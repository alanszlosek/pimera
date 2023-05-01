# TODO
# 
# * convert this to a flask app so we can fetch video JSON via API call
# * return main HTML for / request
# * /videos.json
# * POST /delete/video
#     * this one will simply move the video to a "deleted" folder
#     * we don't actually want ot delete
#     * need to sanitize path to only include alphanum and an extension
#     * trigger soft delete via the delete key .... set those videos to have a red background
#     * trap backspace so we don't actually go back
# 

from flask import g, Flask, request
import glob
import json
import mysql.connector
import os
import re
import time

fp = open('config.json', 'r')
config = json.load(fp)

# i dont remember how this works
offsetPath = '/home/user/projects/surveillance-ui/public'
offsetPath = '/mnt/media/surveillance'

app = Flask(__name__)

def get_db():
    return mysql.connector.connect(user=config.username, password=config.password, host=config.host, database=config.database)
    #return sqlite3.connect('/home/user/projects/surveillance-videos/files.sqlite3')

def tag_sort_key(item):
    return item['tag']

@app.route("/")
def getRoot():
    return "Hello World!"

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

            query = 'SELECT t.id,t.tag, count(vt.tagId) as cnt FROM video_tag AS vt LEFT JOIN tags AS t ON (vt.tagId=t.id) WHERE vt.videoId IN (SELECT vt2.videoId FROM video_tag AS vt2 WHERE vt2.tagId IN (%s) GROUP BY vt2.videoId HAVING count(vt2.videoId)=%d) AND vt.tagId NOT IN (%s) GROUP BY vt.tagId UNION SELECT id,tag,0 as cnt FROM tags' % (tags,num,tags)
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
        #c.execute('SELECT id,tag,(select count(videoId) FROM video_tag WHERE video_tag.tagId = tags.id) as cnt FROM tags WHERE cnt > 0 ORDER BY cnt DESC, tag')
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
    global offsetPath
    #files = glob.glob(basePath + '/*.mp4', recursive=True)
    #files.sort(reverse=True)
    #files = list(map(os.path.basename, files))
    #return json.dumps(files)

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
        if tags != '-1':
            if re.fullmatch('^[0-9]+(,[0-9]+)*$', tags):
                num = len(tags.split(','))
                c.execute('SELECT id,path,createdAt FROM videos WHERE status=0 AND id IN (SELECT videoId FROM video_tag WHERE tagId IN (%s) GROUP BY videoId HAVING count(tagId)=%%s ) ORDER BY createdAt DESC LIMIT %%s,%%s' % (tags,), (num, offset, limit))
            else:
                c.execute('SELECT id,path,createdAt FROM videos WHERE status=0 ORDER BY createdAt DESC LIMIT %s,%s', (offset, limit))
        else:
            # return videos without any tags
            c.execute('SELECT id,path,createdAt FROM videos WHERE status=0 AND id NOT IN (SELECT DISTINCT videoId FROM video_tag) ORDER BY createdAt DESC LIMIT %s,%s', (offset, limit))
    else:
        c.execute('SELECT id,path,createdAt FROM videos WHERE status=0 ORDER BY createdAt DESC LIMIT %s,%s', (offset, limit))
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
            'createdAt': row[2]
        })
    db.close()
    return json.dumps(out)


@app.route("/video/archive/<int:locationId>", methods=['POST'])
def archiveVideo(locationId):
    #os.rename(basePath + filename, archivePath + filename)
    db = get_db()
    c = db.cursor()
    c.execute('UPDATE videos SET status=status|1 WHERE id=?', (locationId,))

    db.commit()
    db.close()
    return '[]'

@app.route("/video/tag/<int:locationId>", methods=['POST'])
def tagVideo(locationId):
    posted = request.get_json()
    tagId = posted['tagId']
    #os.rename(basePath + filename, archivePath + filename)
    db = get_db()
    c = db.cursor()
    c.execute('SELECT id FROM videos WHERE id=?', (locationId,))
    row = c.fetchone()
    videoId = row[0]
    
    print(tagId)
    
    c.execute('SELECT tagId FROM video_tag WHERE id=? AND tagId=?', (videoId,tagId))
    rows = c.fetchall()
    print(rows)
    if len(rows) == 0:
        # add tag
        c.execute('INSERT INTO video_tag (tagId,id) VALUES(?,?)', (tagId,videoId))
    elif len(rows) == 1:
        # remove tag
        c.execute('DELETE FROM video_tag WHERE id=? AND tagId=?', (videoId,tagId))

    db.commit()
    
    c.execute('SELECT t.tag FROM video_tag AS ft LEFT JOIN tags AS t ON (ft.tagId=t.id) WHERE ft.id=? ORDER BY t.tag', (videoId,))
    out = {
        'tags':[]
    }
    for row in c:
        out['tags'].append(row[0])
       
    db.close()
    return json.dumps(out)
    
if __name__ == '__main__':
    app.run(host='localhost', port=5004, debug=True)

