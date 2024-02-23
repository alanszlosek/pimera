import datetime
import glob
import hashlib
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

for row in rows:
    filepath = os.path.join(basePath, row[0][1:-4]) + '*'
    print(f"Checking {filepath}")
    for f in glob.glob(filepath):
        print('Removing %s' % (f,))
        try:
            os.remove(f)
        except Exception as e:
            a = True

