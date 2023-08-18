from PIL import Image
import cv2
import datetime
import json
import math
import mysql.connector
import numpy as np
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
# folder where video posters and object detection result images should be saved
thumbnailBasePath = config['thumbnailPath']


def get_connection():
    global config
    return mysql.connector.connect(user=config['username'], password=config['password'], host=config['host'], database=config['database'])


class Detection:
    def __init__(self, config):
        self.hostname = socket.gethostname()
        self.statsd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
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
                skip = math.floor(20 / 4)

            # TODO: can we fetch from URL ... YES IT APPEARS SO
            videoFile = basePath + row['path']
            cap = cv2.VideoCapture(videoFile)
            if not cap or not cap.isOpened():
                print('Failed to open %s' % (videoFile,))
                dbCursor = db.cursor(dictionary=True)
                dbCursor.execute('UPDATE videos SET objectDetectionRan=1,objectDetectionRanAt=%s,objectDetectionRunSeconds=%s WHERE id=%s', (datetime.datetime.now().timestamp(), 0, rowId))
                db.commit()
                dbCursor.close()

                row = self.NextRow(db)
                continue


            print('Opened and processing: %s' % videoFile)
            self.final_classes = dict()
            self.class_images = dict()


            #skipFrame = False
            frameCount = 0
            batch = []
            batchSize = 5
            while True:
                # Read frame from video
                ret, image_np = cap.read()
                if ret == False:
                    if len(batch) > 0:
                        self.ProcessBatch(batch)
                    print('Done')
                    break
                
                # capture frame at 1 second out to represent the video
                if frameCount == fps:
                    print('Saving poster thumbnail')
                    p = pathlib.PurePath(videoFile)
                    thumbnailPath = os.path.join(thumbnailBasePath, "%s.jpg" % (p.stem,) )
                    cv2.imwrite(thumbnailPath, image_np)
                

                if frameCount % skip == 0:
                    image_ar = np.asarray(image_np)
                    batch.append(image_ar)
                    if len(batch) >= batchSize:
                        self.ProcessBatch(batch)
                        batch = []

                frameCount += 1
            
            # Process remaining frames
            if len(batch) > 0:
                self.ProcessBatch(batch)
                batch = []

            # end frame loop
            cap.release()

            print('Found in video: ' + ','.join(self.final_classes))

            dbCursor2 = db.cursor(dictionary=True)
            for tfClass in self.final_classes:
                # make sure tag exists in tags table
                dbCursor2.execute('SELECT id FROM tags WHERE tag=%s', (tfClass,))
                tag = dbCursor2.fetchone()
                if tag:
                    tagId = tag['id']
                else:
                    print('Tag not found in DB, pre-creating: %s' % (tfClass,))
                    dbCursor2.execute('INSERT INTO tags (tag) VALUES(%s)', (tfClass,))
                    tagId = dbCursor2.lastrowid

                dbCursor2.execute('REPLACE INTO video_tag (tagId,videoId,confidence,taggedBy) VALUES(%s,%s,%s,%s)', (tagId,rowId, self.final_classes[tfClass], 2))

                p = pathlib.PurePath(videoFile)
                imagePath = "%s/%s_%s.jpg" % (thumbnailBasePath, p.stem, tfClass)
                cv2.imwrite(imagePath, self.class_images[tfClass])
            db.commit()
            dbCursor2.close()

            elapsed = time.time() - st

            # update locations to signal we've run object detection on this file
            dbCursor = db.cursor(dictionary=True)
            dbCursor.execute('UPDATE videos SET objectDetectionRan=1,objectDetectionRanAt=%s,objectDetectionRunSeconds=%s WHERE id=%s', (datetime.datetime.now().timestamp(), elapsed, rowId))
            db.commit()
            dbCursor.close()

            # TODO: pull this out
            h = socket.gethostname()
            m = "surveillance.tensorflow_run,host=%s:1|c\nsurveillance.tensorflow_duration,host=%s:%d|ms"  % (h, h, math.ceil(elapsed * 1000))
            self.statsd.sendto(m.encode('utf8'), ('192.168.1.173', 8125))

            row = self.NextRow(db)
        db.close()

    def ProcessBatch(self, batch):
        batchLength = len(batch)
        #print('Detecting batch of %d' % (batchLength,))
        results = self.model(batch)

        m = "pimera.object_detection,host=%s:%d|c"  % (self.hostname, batchLength)
        self.statsd.sendto(m.encode('utf8'), ('192.168.1.173', 8125))

        #print('Got %d results' % (len(results), ))

        for result in results:
            classNames = result.names
            for box in result.boxes:
                classId = int(box.cls[0])
                confidence = int(box.conf[0] * 100)
                className = classNames[ classId ]
                if className == 'bench':
                    continue

                if confidence < 30:
                    continue
                if not className in self.final_classes or confidence > self.final_classes[className]:
                    self.final_classes[ className ] = confidence
                    self.class_images[ className ] = result.plot()
        
        self.batch = []



d = Detection(config)
d.Run()




