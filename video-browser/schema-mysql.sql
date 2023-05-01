CREATE TABLE videos (
    id bigint unsigned primary key auto_increment,
    path varchar(255), -- relative path
    createdAt bigint unsigned not null default 0,
    
    -- bitwise status: 0 visible, 1 hidden, 2 deleted
    status tinyint unsigned not null default 0,
    hiddenAt bigint unsigned not null default 0,
    deletedAt bigint unsigned not null default 0,
    objectDetectionRanAt bigint unsigned not null default 0,
    objectDetectionRunSeconds int unsigned not null default 0,
    objectDetectionRan tinyint unsigned not null default 0
);
CREATE INDEX video_path on videos (path);
CREATE INDEX video_status on videos (status);
CREATE INDEX video_objectDetectionRan on videos (objectDetectionRan);


CREATE TABLE video_tag (
    videoId bigint,
    tagId bigint,
    taggedBy smallint unsigned not null default 0,
    confidence tinyint unsigned not null default 0
);
CREATE UNIQUE INDEX tagId_videoId on video_tag (tagId,videoId);
CREATE UNIQUE INDEX video_tag_videoId on video_tag (videoId);
CREATE UNIQUE INDEX video_tag_tagId on video_tag (tagId);

