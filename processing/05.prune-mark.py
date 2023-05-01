import datetime
import hashlib
import json
import os
import re
import mysql.connector
import sys

fp = open('config.json', 'r')
config = json.load(fp)

my = mysql.connector.connect(user=config.username, password=config.password, host=config.host, database=config.database)
c = my.cursor()

# THESE ARE OLD
queries = [
    # mark videos without any tags
    "update videos as v1 set v1.status=2 where v1.status<>2 and v1.id in (select v2.id from videos as v2 where v2.objectDetectionRan=1 and v2.id not in (select distinct t.videoId from video_tag as t))",

    # tagged only with raspi4: 64
    "update videos as v1 set v1.status=2 where v1.status<>2 and v1.objectDetectionRan=1 and v1.id in (select t1.videoId from video_tag as t1 where t1.videoId in (select distinct t2.videoId from video_tag as t2 where t2.tagId=64) group by t1.videoId having count(t1.videoId) = 1)",

    # tagged only with car and raspi4
    "update videos set status=2 where status<>2 and objectDetectionRan=1 and id in (select videoId from video_tag where videoId in (select videoId from video_tag where tagId=1 UNION select videoId from video_tag where tagId=64) group by videoId having count(videoId) = 2)",

    # tagged only with truck and raspi4
    "update videos set status=2 where status<>2 and objectDetectionRan=1 id in (select videoId from video_tag where videoId in (select videoId from video_tag where tagId=6 UNION select videoId from video_tag where tagId=64) group by videoId having count(videoId) = 2)",

    # tagged only with car, truck, raspi4
    "update videos set status=2 where status<>2 and objectDetectionRan=1 and id in (select videoId from video_tag where videoId in (select videoId from video_tag where tagId=1 UNION select videoId from video_tag where tagId=6 UNION select videoId from video_tag where tagId=64) group by videoId having count(videoId) = 2)",

    # tagged only with car, truck, boat
    "update videos set status=2 where status<>2 and objectDetectionRan=1 id in (select videoId from video_tag where videoId in (select videoId from video_tag where tagId=1 UNION select videoId from video_tag where tagId=6 UNION select videoId from video_tag where tagId=9 UNION select videoId from video_tag where tagId=64) group by videoId having count(videoId) = 3)"
]

tags = [
    # no tags
    [],
    # just raspi4
    [64],
    # raspi4 and car
    [64,1],
    # raspi4 and truck
    [64,6],
    # raspi4, car, truck
    [64,1,6],
    # car, truck boat
    [1,6,9],
    # raspi4, bench
    [64,16]
]

def unionPart(id):
    return "SELECT videoId FROM video_tag WHERE tagId=%s" % (id,)

c = my.cursor()
for tagIds in tags:
    if len(tagIds) > 0:
        parts = map(unionPart, tagIds)
        subquery = " UNION ".join(parts)

        #c.execute(sql)
        #tagIds = set()
        #for row in c:
        #    tagIds.append( row['videoId'] )

        sql = "select videoId from video_tag where videoId in (%s) GROUP BY videoId HAVING COUNT(videoId) = %d" % (subquery, len(tagIds))
        #print(sql)

        ids = set()
        c.execute(sql)
        for row in c:
            ids.add( row[0] )
        #print(ids)

        clause = "(%s)" % ( ','.join(map(str,ids)), )
        sql = "update videos set status=2 where status<>2 and objectDetectionRan=1 and id in %s" % (clause,)
        #print(sql)
        c.execute(sql)
        my.commit()

my.close()
