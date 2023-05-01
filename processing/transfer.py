import datetime
import hashlib
import os
import re
import mysql.connector
import sqlite3
import sys


sqlite = sqlite3.connect('/home/user/projects/pimera/files.sqlite3')
my = mysql.connector.connect(user='pimera', password='pimera', host='192.168.1.173', database='pimera')
m = my.cursor()

# fetch all videos from database
s = sqlite.cursor()
#s.execute("SELECT id,path,fileCreatedAt,objectDetectionRanAt,objectDetectionRunSeconds FROM locations ORDER BY id")
#files = s.fetchall()
files = []
for row in files:
    row = list(row)
    row[1] = row[1].replace('/mnt/media/surveillance', '')
    row[2] = int(row[2])
    row[3] = int(row[3])
    row[4] = int(row[4])
    print(row)
    if row[2] < 0:
        row[2] = 0
    m.execute('INSERT INTO videos (id,path,createdAt,objectDetectionRanAt,objectDetectionRunSeconds) VALUES(%s,%s,%s,%s,%s)', row)
 

s = sqlite.cursor()
s.execute("SELECT locations.id,file_tag.tagId,file_tag.taggedBy from file_tag left join locations on (file_tag.fileSha1=locations.sha1)")
row = s.fetchone()
while row:
    print(row)
    m.execute('INSERT INTO video_tag (videoId,tagId,taggedBy) VALUES(%s,%s,%s)', row)
    my.commit()
    row = s.fetchone()

print('Done')

