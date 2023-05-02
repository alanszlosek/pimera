import datetime
import hashlib
import json
import os
import re
import mysql.connector
import sqlite3
import sys

fp = open('../config.json', 'r')
config = json.load(fp)

sqlite = sqlite3.connect('/home/user/projects/pimera/files.sqlite3')
my = mysql.connector.connect(user=config['username'], password=config['password'], host=config['host'], database=config['database'])
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

