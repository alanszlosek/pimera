import json
import mysql.connector
import os
import re

fp = open('../config.json', 'r')
config = json.load(fp)


def get_db():
    global config
    return mysql.connector.connect(user=config['username'], password=config['password'], host=config['host'], database=config['database'])

def tag_sort_key(item):
    return item['tag']


def get_files(query):
    db = get_db()
    c = db.cursor(buffered=True)
    d = db.cursor()
    print(query)

    # check request.args
    offset = 0
    if 'offset' in query:
        offset = int(query['offset'])
    limit = 50
    if 'limit' in query:
        limit = int(query['limit'])


    if 'tagIds' in query:
        tags = query['tagIds']
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
    return out

def get_tags(query):
    db = get_db()
    c = db.cursor()

    # check request.args
    offset = 0
    limit = 50

    out = []
    if 'tagIds' in query:
        tags = query['tagIds']
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
    return out
