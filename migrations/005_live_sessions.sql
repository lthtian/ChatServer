-- 直播只保存业务状态，音视频内容由独立媒体服务接入和分发。
CREATE TABLE LiveSession (
    live_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,
    owner_id INT NOT NULL,
    client_msg_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    chatkey VARCHAR(50) COLLATE utf8mb4_unicode_ci NOT NULL,
    isgroup TINYINT(1) NOT NULL,
    title VARCHAR(120) NOT NULL,
    state ENUM('waiting','live','interrupted','ended') NOT NULL DEFAULT 'waiting',
    publisher_id VARCHAR(96) CHARACTER SET ascii COLLATE ascii_bin NOT NULL DEFAULT '',
    created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
    UNIQUE KEY uq_live_request (owner_id,client_msg_id),
    KEY idx_live_owner (owner_id,state),
    CONSTRAINT fk_live_owner FOREIGN KEY (owner_id) REFERENCES User(id),
    CONSTRAINT ck_live_group CHECK (isgroup IN (0,1))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

ALTER TABLE History
    ADD COLUMN live_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL,
    ADD UNIQUE KEY uq_history_live(live_id),
    ADD CONSTRAINT fk_history_live FOREIGN KEY(live_id) REFERENCES LiveSession(live_id),
    MODIFY kind ENUM('text','image','file','live') NOT NULL DEFAULT 'text',
    DROP CHECK ck_history_media,
    ADD CONSTRAINT ck_history_media CHECK (
        (kind='text' AND media_id IS NULL AND live_id IS NULL)
        OR (kind IN ('image','file') AND media_id IS NOT NULL AND live_id IS NULL AND client_msg_id IS NOT NULL)
        OR (kind='live' AND media_id IS NULL AND live_id IS NOT NULL AND client_msg_id IS NOT NULL)
    );
