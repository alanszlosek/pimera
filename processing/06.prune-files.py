import datetime
import hashlib
import json
import os
import re
import mysql.connector
import sys

fp = open('config.json', 'r')
config = json.load(fp)

basePath = sys.argv[1]
print(basePath)

my = mysql.connector.connect(user=config.username, password=config.password, host=config.host, database=config.database)
c = my.cursor()

c.execute('select path from videos where status=2')
for row in c:
    filepath = "%s%s" % (basePath, row[0])
    print("Removing %s" % (filepath,))
    os.remove(filepath)

my.close()
