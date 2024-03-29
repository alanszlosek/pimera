from flask import g, Flask, request, send_from_directory
import glob
import json
import mysql.connector
import os
import re
import signal
import socket
import threading
import time

fp = open('../config.json', 'r')
config = json.load(fp)

videoPath = config['videoPath']

cameras = {} # holds list of cameras that have announced recently
app = Flask(__name__)

def get_db():
    fp = open('../config.json', 'r')
    config = json.load(fp)
    return mysql.connector.connect(user=config['username'], password=config['password'], host=config['host'], database=config['database'])

def tag_sort_key(item):
    return item['tag']

@app.route("/")
def getRoot():
    return send_from_directory('public', 'index.html')

@app.route("/tags")
def getTags():
    db = get_db()
    c = db.cursor()

    # check request.args
    offset = 0
    limit = 50

    out = []
    if 'tagIds' in request.args:
        tags = request.args['tagIds']
        if re.fullmatch('^[0-9]+(,[0-9]+)*$', tags):
            num = len(tags.split(','))
            print(tags)

            query = 'SELECT t.id,t.tag, count(vt.tagId) as cnt FROM video_tag AS vt LEFT JOIN tags AS t ON (vt.tagId=t.id) WHERE vt.videoId IN (SELECT vt2.videoId FROM video_tag AS vt2 WHERE vt2.tagId IN (%s) GROUP BY vt2.videoId HAVING count(vt2.videoId)=%d) GROUP BY vt.tagId UNION SELECT id,tag,0 as cnt FROM tags' % (tags,num)
            c.execute(query)
            tag_dict = {}
            for row in c:
                id = row[0]
                tag = row[1]
                cnt = int(row[2])
                if not id in tag_dict:
                    tag_dict[id] = {
                        'id': row[0],
                        'tag': row[1],
                        'count': row[2]
                    }
                    out.append( tag_dict[id] )
                else:
                    if cnt > 0:
                        tag_dict[id]['count'] = cnt
            out.sort(key=tag_sort_key)

        else:
            c.execute('SELECT vt.tagId as id,t.tag,count(vt.tagId) as cnt from video_tag as vt LEFT JOIN tags as t on (vt.tagId=t.id) GROUP BY vt.tagId ORDER BY tag')
            for row in c:
                out.append({
                    'id': row[0],
                    'tag': row[1],
                    'count': row[2]
                })
    else:

        # most frequently used tag first
        c.execute('SELECT vt.tagId as id,t.tag,count(vt.tagId) as cnt from video_tag as vt LEFT JOIN tags as t on (vt.tagId=t.id) GROUP BY vt.tagId ORDER BY tag')
        for row in c:
            out.append({
                'id': row[0],
                'tag': row[1],
                'count': row[2]
            })
    db.close()
    return json.dumps(out)


@app.route("/files")
def getFiles():
    global videoPath

    db = get_db()
    c = db.cursor(buffered=True)
    d = db.cursor()

    # check request.args
    offset = 0
    if 'offset' in request.args:
        offset = int(request.args['offset'])
    limit = 50
    if 'limit' in request.args:
        limit = int(request.args['limit'])


    if 'tagIds' in request.args:
        tags = request.args['tagIds']
        if tags != 'NONE':
            if re.fullmatch('^[0-9]+(,[0-9]+)*$', tags):
                num = len(tags.split(','))
                c.execute('SELECT id,path,createdAt,durationSeconds FROM videos WHERE status=0 AND id IN (SELECT videoId FROM video_tag WHERE tagId IN (%s) GROUP BY videoId HAVING count(tagId)=%%s ) ORDER BY createdAt DESC LIMIT %%s,%%s' % (tags,), (num, offset, limit))
            else:
                c.execute('SELECT id,path,createdAt,durationSeconds FROM videos WHERE status=0 ORDER BY createdAt DESC LIMIT %s,%s', (offset, limit))
        else:
            # return videos without any tags
            c.execute('SELECT id,path,createdAt,durationSeconds FROM videos WHERE status=0 AND id NOT IN (SELECT DISTINCT videoId FROM video_tag) ORDER BY createdAt DESC LIMIT %s,%s', (offset, limit))
    else:
        c.execute('SELECT id,path,createdAt,durationSeconds FROM videos WHERE status=0 ORDER BY createdAt DESC LIMIT %s,%s', (offset, limit))
    out = []
    for row in c:
        d.execute('SELECT t.tag FROM tags AS t LEFT JOIN video_tag AS ft ON (ft.tagId=t.id) WHERE ft.videoId=%s ORDER BY t.tag', (row[0],))
        tags = []
        for tag in d:
            tags.append( tag[0] )
        # fetch tags for each
        out.append({
            'id': row[0],
            'path': row[1],
            'filename': os.path.basename(row[1]),
            'tags': tags,
            'createdAt': row[2],
            'durationSeconds': row[3]
        })
    db.close()
    return json.dumps(out)


@app.route("/cameras.json", methods=['GET'])
def getCameras():
    global cameras
    # prune old cameras
    cutoff = time.time() - 10.0
    for key in cameras:
        ts = cameras[key]
        if ts < cutoff:
            print('removing %s' % key)
            del cameras[key]
    return json.dumps(cameras)


class CameraTracker(threading.Thread):
    def __init__(self):
        threading.Thread.__init__(self)
        self.running = True
        self.start()

    def run(self):
        global cameras
        UDP_IP = "0.0.0.0"
        UDP_PORT = 5001
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((UDP_IP, UDP_PORT))

        while self.running:
            data, addr = sock.recvfrom(1024) # buffer size is 1024 bytes
            #print("data")
            #print("Camera heartbeat %s" % addr[0])
            cameras[ addr[0] ] = time.time()

def signal_handler(sig, frame):
    global cameraTracker
    cameraTracker.running = False
    # how to close flask, too?
    print('Exiting ...')

signal.signal(signal.SIGINT, signal_handler)
signal.signal(signal.SIGTERM, signal_handler)

if __name__ == '__main__':
    cameraTracker = CameraTracker()
    app.run(host='localhost', port=5004, debug=True)

