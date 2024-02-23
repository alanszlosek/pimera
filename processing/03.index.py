import datetime
import hashlib
import json
import os
import re
import mysql.connector
import sys

fp = open('../config.json', 'r')
config = json.load(fp)

#print(sys.argv[1])
#basePath = sys.argv[1]
basePath = config['videoPath']

my = mysql.connector.connect(user=config['username'], password=config['password'], host=config['host'], database=config['database'])


def iterateOverFiles(root):
    for dirpath, dirnames, filenames in os.walk(root):
        for f_name in filenames:
            yield os.path.join(dirpath, f_name)


for filepath in iterateOverFiles(basePath):
    if not filepath[-3:] == 'mp4':
        continue
    # is there a device name in the filename?
    dims = re.search(r'(\d{8})\d{6}_([a-z0-9]+)_(\d+)x(\d+)x(\d+)\.mp4$', filepath)
    if dims:
        cameraName = dims.group(2)
    else:
        cameraName = None

    relativePath = filepath.replace('/mnt/media/surveillance', '')
    # skip staging folder
    if relativePath[:5] == '/h264':
        continue
    stat = os.stat(filepath)

    # get stat info
    # look up file in files table
    # if found
    #   compare filesystem stat info: bytes, created, modified
    #   if those are the same, assume hash hasn't changed
    # if not found
    #   hash the file
    #   if hash not in hashes table
    #     add to hashes
    #     add to files table
    #   if hash in hashes table
    #     add to files table

    # extract timestamp from filename
    filename = os.path.basename(filepath)
    ts = datetime.datetime(
        year=int(filename[0:4]),
        month=int(filename[4:6]),
        day=int(filename[6:8]),
        hour=int(filename[8:10]),
        minute=int(filename[10:12]),
        second=int(filename[12:14]),
        tzinfo=datetime.timezone(datetime.timedelta(0))
    ).timestamp()

    c = my.cursor(dictionary=True)
    c.execute('SELECT id from videos WHERE path=%s', (relativePath,))
    row = c.fetchone()
    if not row:
        print('Found new file. Indexing: ' + filename)
        c.execute('INSERT INTO videos (path,createdAt,sizeBytes) VALUES(%s,%s)', (relativePath, ts, stat.st_size))
        videoId = c.lastrowid

        # add tag for camera name
        if cameraName:
            # make sure tag exists in tags table
            tag = 'camera:' + cameraName
            c.execute('SELECT id FROM tags WHERE tag=%s', (tag,))
            tagRow = c.fetchone()
            if tagRow:
                tagId = tagRow[0]
            else:
                print('Camera tag not found, pre-creating: %s' % (tag,))
                c.execute('INSERT INTO tags (tag) VALUES(%s)', (tag,))
                tagId = c.lastrowid
            c.execute('REPLACE INTO video_tag (tagId,videoId,taggedBy) VALUES(%s,%s,%s)', (tagId,videoId,3))

        my.commit()
    else:
        print('Updating file size for: ' + filename)
        c.execute('UPDATE videos SET sizeBytes=%s WHERE id=%s', (stat.st_size, row['id']))
        my.commit()
        
my.close()
