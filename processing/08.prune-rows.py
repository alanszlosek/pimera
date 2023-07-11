import datetime
import hashlib
import json
import os
import re
import mysql.connector
import sys

fp = open('../config.json', 'r')
config = json.load(fp)

my = mysql.connector.connect(user=config['username'], password=config['password'], host=config['host'], database=config['database'])
c = my.cursor()

# delete tags for files we deleted
c.execute("DELETE FROM video_tag WHERE videoId in (select id from videos WHERE status=2)")
# delete the file rows themselves
c.execute("DELETE FROM videos where status=2")
my.commit()

my.close()
