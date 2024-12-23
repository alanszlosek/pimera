import cv2
import datetime
import json
import math
import mysql.connector
import os
import pathlib
import re
import requests
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

def upload_image(path, data):
    global config
    # path should be "/api/image/20240930/bla.jpg"
    headers = {'content-type': 'image/jpeg'}
    r = requests.post(f"{config['apiHost']}/api/image{path}", data=data, verify=False, headers=headers)


class Detection:
    def __init__(self, config):
        self.hostname = socket.gethostname()
        self.statsd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.statsdHost = config['statsdHost']
        self.model = model = YOLO( config['model'] )
        self.skippedTags = config['skippedTags'] if 'skippedTags' in config else []
    
    def NextRow(self, db):
        dbCursor = db.cursor(dictionary=True)
        dbCursor.execute("SELECT id,path FROM videos WHERE objectDetectionRan = 0 ORDER by RAND() LIMIT 1")
        return dbCursor.fetchone()

    def Run(self):
        global config
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
            thumbnailFilePrefix = row["path"][:-4] # chop off extension

            videoFile = f"{config['apiHost']}/movies{row['path']}"
            print( f"Opened and processing: {videoFile}" )
            results = self.Process(videoFile, skip)
            # TODO: also figure out how to remove file after processing if we fetched from http
            if videoFile.startswith("http"):
                filename = videoFile.split("/").pop()
                print(f"Removing {filename}")
                os.remove(filename)

            # TODO: perhaps if results == False there was an open failure

            print('Found in video: ' + ','.join( results.keys() ))

            dbCursor2 = db.cursor(dictionary=True)
            for detected, result in results.items():
                if detected == '__':
                    # save cover image
                    # imagePath = f"{thumbnailBasePath}.jpg"
                    # cv2.imwrite(imagePath, result['image'])

                    # Instead of writing to disk, post to API to save image
                    result,encimg = cv2.imencode('.jpg',result["image"])
                    filename = f"{thumbnailFilePrefix}.jpg"
                    if result:
                        upload_image(filename, encimg.tobytes())
                    else:
                        print("Failed to encode image: " + filename)
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

                # imagePath = f"{thumbnailBasePath}_{detected}.jpg"
                # cv2.imwrite(imagePath, result['image'])
                # Instead of writing to disk, post to API to save image

                result,encimg = cv2.imencode('.jpg',result["image"])
                filename = f"{thumbnailFilePrefix}_{detected}.jpg"
                if result:
                    upload_image(filename, encimg.tobytes())
                else:
                    print("Failed to encode image: " + filename)
            db.commit()
            dbCursor2.close()

            elapsed = time.time() - st

            detections = len(results) - 1
            # update locations to signal we've run object detection on this file
            dbCursor = db.cursor(dictionary=True)
            dbCursor.execute('UPDATE videos SET objectDetectionRan=1,objectDetectionRanAt=%s,objectDetectionRunSeconds=%s WHERE id=%s', (datetime.datetime.now().timestamp(), elapsed, rowId))
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

                if className in self.skippedTags:
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
