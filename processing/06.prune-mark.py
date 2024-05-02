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
        tagIds_str = ",".join(map(str,tagIds))
        clauses = [
            # start with clause to make sure video doesn't have tags outside of the list we care about
            f"videoId not in (SELECT videoId from video_tag where tagId NOT IN ({tagIds_str}))"
        ]
        for tagId in tagIds:
            clauses.append(f"videoId in (SELECT videoId FROM video_tag WHERE tagId={tagId})")
        #print(clauses)

        clauses2 = ' AND '.join(clauses)

        sql = f"select videoId from video_tag where {clauses2}"
        #print(sql)

        ids = set()
        c.execute(sql)
        for row in c:
            ids.add( row[0] )
        #print(ids)

        if not len(ids):
            continue

        clause = "(" + ','.join(map(str,ids)) + ")"
        sql = "update videos set status=2 where status<>2 and objectDetectionRan=1 and id in %s" % (clause,)
        #print(sql)
        c.execute(sql)
        my.commit()

my.close()
