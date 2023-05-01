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

s = sqlite.cursor()
s.execute("SELECT locations.id,file_tag.tagId,file_tag.taggedBy from file_tag left join locations on (file_tag.fileSha1=locations.sha1)")
row = s.fetchone()
while row:
    print(row)
    m.execute('INSERT INTO video_tag (videoId,tagId,taggedBy) VALUES(%s,%s,%s)', row)
    my.commit()
    row = s.fetchone()

print('Done')

