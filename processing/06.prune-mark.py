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


def getTagIds(tags: list[int]):
    global my
    names = "'" + "','".join(tags) + "'"
    sql = f"SELECT id FROM tags WHERE tag IN ({names})"
    rows = my.cursor()
    rows.execute(sql)

    out = []
    for row in rows.fetchall():
        out.append( str(row[0]) )
    return out

def unionPart(id):
    return "SELECT videoId FROM video_tag WHERE tagId=%s" % (id,)

def markForDeletion(videoIds):
    c = my.cursor()
    n = 500
    while len(videoIds) > 0:
        ids = ",".join(videoIds[0:n])

        sql = f"UPDATE videos SET status=2 WHERE id in ({ids})"
        print(sql)
        c.execute(sql)
        del videoIds[0:n]


cameraTags = ["camera:raspi4", "camera:raspi3", "camera:raspi3b", "camera:raspi02", "camera:raspi0"]
cameraTagIds = getTagIds(cameraTags)

# if only a combination of these (plus camera specifier)
anyTags = ["car", "truck", "boat", "sheep", "bench", "potted plant", "cow", "bear", "umbrella"]
# map tag names to ids
anyTagIds = getTagIds(anyTags)

# Must have top level key as tag, but can have any of the others
# CANNOT have any tags outside of must or any list
mustToAny = {}
for cameraTag in cameraTagIds:
    other = anyTagIds.copy()
    other.append(cameraTag)
    mustToAny[ cameraTag ] = other



# TODO: refactor this
for mustHaveTagId, anyTagIds in mustToAny.items():
    anyTagIdsStr = ",".join(map(str,anyTagIds))
    clauses = [
        # start with clause to make sure video doesn't have tags outside of the list we care about (including the camera tag)
        f"id NOT IN (SELECT DISTINCT videoId from video_tag where tagId NOT IN ({anyTagIdsStr}))",

        # Now ensure it does have any of the tags we do care about
        f"id IN (SELECT DISTINCT videoId from video_tag where tagId IN ({anyTagIdsStr}))"
    ]
    clauses2 = ' AND '.join(clauses)

    sql = f"SELECT id FROM videos WHERE status<>2 and objectDetectionRan=1 AND {clauses2}"
    sql = f"SELECT id FROM videos WHERE status<>2 AND {clauses2}"
    print(sql)
    c = my.cursor()
    c.execute(sql)
    videoIds = []
    for row in c.fetchall():
        videoIds.append( str(row[0]) )
    markForDeletion(videoIds)

    print()
    # continue

    # c.execute(sql)
    # my.commit()


# New query for flagging videos with ONLY low confidence tags
if True:
    temp = ",".join(cameraTagIds)
    print(f"Flagging videos with ONLY low-confidence tags other than {temp}")

    sql = f"""SELECT id FROM videos WHERE
    status<>2 and objectDetectionRan=1
    AND
    id in (SELECT videoId FROM video_tag WHERE confidence > 0 and confidence < 70)
    AND
    id NOT IN (SELECT videoId FROM video_tag WHERE confidence >= 70 and tagId NOT IN ({temp}))"""
    print(sql)

    c = my.cursor()
    c.execute(sql)
    videoIds = []
    for row in c.fetchall():
        videoIds.append( str(row[0]) )
    markForDeletion(videoIds)


# Mark videos with no tags
# Get ids of videos that don't have an id in video_tag
sql = f"""SELECT id FROM videos WHERE
status<>2
AND
id NOT IN (SELECT DISTINCT videoId FROM video_tag)"""
print(sql)

c = my.cursor()
c.execute(sql)
videoIds = []
for row in c.fetchall():
    videoIds.append( str(row[0]) )
markForDeletion(videoIds)


my.commit()
my.close()
