import cv2
import datetime
import json
import math
import mysql.connector
import os
import pathlib
import re
import socket
import time
from ultralytics import YOLO

fp = open('../config.json', 'r')
config = json.load(fp)

# configure base path from which to fetch files
# this will be a prefix to the videos.path value from the database
basePath = config['videoPath']

def get_connection():
    global config
    return mysql.connector.connect(user=config['username'], password=config['password'], host=config['host'], database=config['database'])


class Detection:
    def __init__(self, config):
        self.hostname = socket.gethostname()
        self.statsd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.statsdHost = config['statsdHost']
        self.model = model = YOLO( config['model'] )
    
    def NextRow(self, db):
        dbCursor = db.cursor(dictionary=True)
        dbCursor.execute("SELECT id,path FROM videos WHERE objectDetectionRan = 0 ORDER by RAND() LIMIT 1")
        return dbCursor.fetchone()

    def Run(self):
        db = get_connection()

        row = self.NextRow(db)
        while row:
            rowId = row['id']
            st = time.time()

            print('')
            print('==== ==== ====')
            print('')

            # Do object detection 4x per second, so calculate how many frames we need to skip during processing
            dims = re.search(r'(\d+)x(\d+)x(\d+)', row['path'])
            if dims:
                fps = int(dims.group(3))
                skip = math.floor(fps / 4)
            else:
                # TODO: write script to make filenames consistent, then remove this branch
                fps = 20
                skip = math.floor(20 / 4)

            videoFile = basePath + row['path']
            thumbnailBasePath = videoFile[:-4] # excluding the ".mp4"

            print( f"Opened and processing: {videoFile}" )
            results = self.Process(videoFile, skip)

            # TODO: perhaps if results == False there was an open failure

            print('Found in video: ' + ','.join( results.keys() ))

            dbCursor2 = db.cursor(dictionary=True)
            for detected, result in results.items():
                if detected == '__':
                    # save cover image
                    imagePath = f"{thumbnailBasePath}.jpg"
                    cv2.imwrite(imagePath, result['image'])
                    continue
                # make sure tag exists in tags table
                dbCursor2.execute('SELECT id FROM tags WHERE tag=%s', (detected,))
                tag = dbCursor2.fetchone()
                if tag:
                    tagId = tag['id']
                else:
                    print( f"Tag not found in DB, pre-creating: {detected}" )
                    dbCursor2.execute('INSERT INTO tags (tag) VALUES(%s)', (detected,))
                    tagId = dbCursor2.lastrowid

                dbCursor2.execute('REPLACE INTO video_tag (tagId,videoId,confidence,taggedBy) VALUES(%s,%s,%s,%s)', (tagId, rowId, result['confidence'], 2))

                imagePath = f"{thumbnailBasePath}_{detected}.jpg"
                cv2.imwrite(imagePath, result['image'])
            db.commit()
            dbCursor2.close()

            elapsed = time.time() - st

            detections = len(results) - 1
            # since we do four object detections every second
            durationSeconds = detections / 4
            # update locations to signal we've run object detection on this file
            dbCursor = db.cursor(dictionary=True)
            dbCursor.execute('UPDATE videos SET objectDetectionRan=1,objectDetectionRanAt=%s,objectDetectionRunSeconds=%s,durationSeconds=%s WHERE id=%s', (datetime.datetime.now().timestamp(), elapsed, durationSeconds, rowId))
            db.commit()
            dbCursor.close()

            duration = math.ceil(elapsed * 1000)
            m = f"pimera.detections,host={self.hostname}:{detections}|c\npimera.detection_duration,host={self.hostname}:{duration}|ms"
            self.statsd.sendto(m.encode('utf8'), (self.statsdHost, 8125))

            row = self.NextRow(db)
        db.close()

    def Process(self, filename, frame_stride):
        # TODO: handle open failures
        results = self.model(filename, vid_stride=frame_stride)

        out = {}
        for i,result in enumerate(results):
            if i == 0:
                # TODO: save thumbnail as cover image
                print("keeping first result image as poster")
                out['__'] = {
                    "image": result.orig_img, #maybe?
                    "confidence": 0
                }
                continue

            classNames = result.names
            for box in result.boxes:
                classId = int(box.cls[0])
                confidence = int(box.conf[0] * 100)
                className = classNames[ classId ]

                if className == 'bench':
                    continue
                if confidence < 30:
                    continue

                if not className in out or confidence > out[className]['confidence']:
                    out[ className ] = {
                        "image": result.plot(),
                        "confidence": confidence
                    }
        return out


d = Detection(config)
d.Run()

