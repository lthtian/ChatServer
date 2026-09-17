-- ABOUTME: Defines media metadata and durable image message associations.
-- ABOUTME: Applies once to a reviewed MySQL schema before enabling image messages.

CREATE TABLE Media (
    media_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    owner_id INT NOT NULL,
    client_msg_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    chatkey VARCHAR(50) COLLATE utf8mb4_unicode_ci NOT NULL,
    isgroup TINYINT(1) NOT NULL,
    state ENUM('uploading', 'processing', 'ready', 'failed', 'canceled') NOT NULL,
    provider VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    original_key VARCHAR(240) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    thumbnail_key VARCHAR(240) CHARACTER SET ascii COLLATE ascii_bin NULL,
    expected_bytes BIGINT UNSIGNED NOT NULL,
    actual_bytes BIGINT UNSIGNED NULL,
    sha256 CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    mime VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL,
    width INT UNSIGNED NULL,
    height INT UNSIGNED NULL,
    thumbnail_mime VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL,
    thumbnail_bytes BIGINT UNSIGNED NULL,
    thumbnail_width INT UNSIGNED NULL,
    thumbnail_height INT UNSIGNED NULL,
    failure_code VARCHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL,
    expires_at TIMESTAMP(6) NOT NULL,
    created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
        ON UPDATE CURRENT_TIMESTAMP(6),
    PRIMARY KEY (media_id),
    UNIQUE KEY uq_media_request (owner_id, client_msg_id),
    UNIQUE KEY uq_media_object (provider, original_key),
    KEY idx_media_expiry (state, expires_at),
    CONSTRAINT fk_media_owner FOREIGN KEY (owner_id) REFERENCES User(id),
    CONSTRAINT ck_media_conversation CHECK (isgroup IN (0, 1)),
    CONSTRAINT ck_media_provider CHECK (provider IN ('disk', 'oss')),
    CONSTRAINT ck_media_size CHECK (expected_bytes > 0 AND expected_bytes <= 20971520),
    CONSTRAINT ck_media_ready CHECK (
        state <> 'ready' OR (
            actual_bytes IS NOT NULL AND actual_bytes = expected_bytes
            AND mime IS NOT NULL AND mime IN ('image/jpeg', 'image/png')
            AND width IS NOT NULL AND width > 0
            AND height IS NOT NULL AND height > 0
            AND thumbnail_key IS NOT NULL
            AND thumbnail_mime IS NOT NULL AND thumbnail_mime IN ('image/jpeg', 'image/png')
            AND thumbnail_bytes IS NOT NULL AND thumbnail_bytes > 0
            AND thumbnail_width IS NOT NULL AND thumbnail_width BETWEEN 1 AND 320
            AND thumbnail_height IS NOT NULL AND thumbnail_height BETWEEN 1 AND 320
        )
    )
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

ALTER TABLE History
    ADD COLUMN kind ENUM('text', 'image') NOT NULL DEFAULT 'text',
    ADD COLUMN media_id CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL,
    ADD COLUMN client_msg_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL,
    ADD UNIQUE KEY uq_history_request (userid, client_msg_id),
    ADD UNIQUE KEY uq_history_media (media_id),
    ADD KEY idx_history_conversation (isgroup, chatkey, id),
    ADD CONSTRAINT fk_history_media FOREIGN KEY (media_id) REFERENCES Media(media_id),
    ADD CONSTRAINT ck_history_media CHECK (
        (kind = 'text' AND media_id IS NULL)
        OR (kind = 'image' AND media_id IS NOT NULL AND client_msg_id IS NOT NULL)
    );
