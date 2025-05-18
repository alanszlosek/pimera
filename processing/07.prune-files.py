import datetime
import json
import os
import pathlib
import re
import mysql.connector
import sys

fp = open('../config.json', 'r')
config = json.load(fp)

basePath = config['videoPath'] #'/mnt/media/surveillance'
print(basePath)

my = mysql.connector.connect(user=config['username'], password=config['password'], host=config['host'], database=config['database'])
c = my.cursor()

c.execute('select path from videos where status=2')
rows = c.fetchall()
my.close()

videosToPrune = set()
for row in rows:
    withoutExtension = row[0][1:-4]
    videosToPrune.add(withoutExtension)

for root, dirs, files in os.walk(basePath):
    if len(files) > 0:
        # root will be something like "/mnt/camvids/20250515"
        # relativeRoot should end up with something like "20250515/20250515010101_raspi3_100x100x20_tagName.jpg"
        relativeRoot = root.removeprefix(basePath).strip("/\\")
        for file in files:
            # Strip off tag and extension portion of filename
            # This should result in something like "20250515/20250515010101_raspi3_100x100x20"
            withoutTagOrExtension = "_".join(file[:-4].split("_")[0:3])

            relativePathWithoutExtension = os.path.join(relativeRoot, withoutTagOrExtension)
            if file.startswith(relativeRoot) and relativePathWithoutExtension in videosToPrune:
                filepath = os.path.join(root, file)
                print(f"Removing: {file}\n")
                try:
                    os.remove(filepath)
                except:
                    a=True


