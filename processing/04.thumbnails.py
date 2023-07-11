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

fp = open('../config.json', 'r')
config = json.load(fp)

basePath = sys.argv[1]

my = mysql.connector.connect(user=config['username'], password=config['password'], host=config['host'], database=config['database'])
c = my.cursor()

selectCursor = my.cursor(dictionary=True)

def nextRow(selectCursor)
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
        db.commit()

        row = nextRow(selectCursor)



    cap = cv2.VideoCapture(videoFile)
    if not cap or not cap.isOpened():
        row = nextRow(selectCursor)
    i = 0
    while True:
        # Read frame from camera
        ret, image_np = cap.read()
        if ret == False:
            bail
        if i == fps:
            save this one as thumbnail
            p = pathlib.PurePath(videoFile)
            thumbnailPath = os.path.join(basePath, "%.jpg" % (p.stem,) )
            cv2.imwrite(imagePath, image_np)
            cap.close()

            updateCursor = my.cursor(dictionary=True)
            updateCursor.execute('UPDATE videos SET hasThumbnail=1 WHERE id=%s', (row['id'],))
            db.commit()
            break
        i = i + 1

    # select next row
    row = nextRow(selectCursor)

db.commit()
selectCursor.close()