CREATE TABLE videos (
    id integer primary key autoincrement,
    path text, -- relative path
    createdAt unsigned integer not null,
    
    -- bitwise status: 0 visible, 1 hidden, 2 deleted
    status integer not null default 0,
    hiddenAt integer not null default 0,
    deletedAt integer not null default 0,
    objectDetectionRanAt unsigned integer not null default 0,
    objectDetectionRunSeconds unsigned integer not null default 0
);
CREATE INDEX video_path on videos (path);


CREATE TABLE video_tag (
    videoId integer,
    tagId integer,
    taggedBy unsigned integer not null default 0,
    confidence unsigned integer not null default 0
);
CREATE UNIQUE INDEX tagId_videoId on video_tag (tagId,videoId);

