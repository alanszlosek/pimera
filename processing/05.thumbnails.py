from PIL import Image
import cv2
import datetime
import hashlib
import json
import os
import pathlib
import re
import mysql.connector
import sys

# NOTE THIS SCRIPT IS OLD, AND ASSUMPTIONS MAY NOT HOLD ANYMORE













fp = open('../config.json', 'r')
config = json.load(fp)

# configure base path from which to fetch files
# this will be a prefix to the videos.path value from the database
basePath = config['videoPath']
# folder where video posters and object detection result images should be saved
thumbnailBasePath = config['thumbnailPath']

my = mysql.connector.connect(user=config['username'], password=config['password'], host=config['host'], database=config['database'])
c = my.cursor()

selectCursor = my.cursor(dictionary=True)

def nextRow(selectCursor):
    rowQuery = "SELECT id,path FROM videos WHERE hasThumbnail = 0 AND createdAt >= unix_timestamp('2020-06-20') ORDER by RAND() LIMIT 1"
    selectCursor.execute(rowQuery)
    return selectCursor.fetchone()

row = nextRow(selectCursor)
while row:
    rowId = row['id']

    dims = re.search(r'(\d+)x(\d+)x(\d+)', row['path'])
    if dims:
        fps = int(dims.group(3))
    else:
        print('ERROR: Cant find fps in row path')
        updateCursor = my.cursor(dictionary=True)
        updateCursor.execute('UPDATE videos SET hasThumbnail=2 WHERE id=%s', (row['id'],))
        my.commit()
        row = nextRow(selectCursor)
        continue


    # skip leading slash from row, since os.path.join behavior is quirky
    videoFile = os.path.join(basePath, row['path'][1:])
    print('Opening ' + videoFile)
    try:
        cap = cv2.VideoCapture(videoFile)
    except Exception as e:
        print('ERROR: OpenCV exception: ' + e)
        updateCursor = my.cursor(dictionary=True)
        updateCursor.execute('UPDATE videos SET hasThumbnail=2 WHERE id=%s', (row['id'],))
        my.commit()
        row = nextRow(selectCursor)
        continue

    if not cap or not cap.isOpened():
        print('ERROR: OpenCV failure with ' + videoFile)
        updateCursor = my.cursor(dictionary=True)
        updateCursor.execute('UPDATE videos SET hasThumbnail=2 WHERE id=%s', (row['id'],))
        my.commit()
        row = nextRow(selectCursor)
        continue
    
    fps = cap.get(cv2.CAP_PROP_FPS)
    frameCount = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    durationSeconds = frameCount/fps

    i = 0
    while True:
        # Read frame from camera
        ret, image_np = cap.read()
        if ret == False:
            # TODO: do we need error handling here?
            break
        # capture from 1 second in
        if i == fps:
            # save this one as thumbnail
            p = pathlib.PurePath(videoFile)
            thumbnailPath = os.path.join(thumbnailBasePath, "%s.jpg" % (p.stem,) )
            cv2.imwrite(thumbnailPath, image_np)
            cap.release()

            updateCursor = my.cursor(dictionary=True)
            updateCursor.execute('UPDATE videos SET hasThumbnail=1,durationSeconds=%s WHERE id=%s', (durationSeconds, row['id']))
            my.commit()
            break
        i = i + 1

    # select next row
    row = nextRow(selectCursor)

my.commit()
selectCursor.close()
