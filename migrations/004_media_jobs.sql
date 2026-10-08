-- 原件就绪与转码任务入队在同一事务中完成；一份原件只对应一个处理任务。
CREATE TABLE MediaJob (
    media_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,
    state ENUM('queued','processing','ready','failed','canceled') NOT NULL DEFAULT 'queued',
    attempts INT UNSIGNED NOT NULL DEFAULT 0,
    failure_code VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    KEY idx_media_job_queue (state, created_at),
    CONSTRAINT fk_job_media FOREIGN KEY (media_id) REFERENCES Media(media_id) ON DELETE CASCADE
) ENGINE=InnoDB;

INSERT INTO MediaJob(media_id)
SELECT media_id FROM Media WHERE state='ready' AND kind='file' AND LOWER(RIGHT(name,4))='.mp4'
AND (expires_at>CURRENT_TIMESTAMP(6) OR EXISTS(SELECT 1 FROM History WHERE History.media_id=Media.media_id));
