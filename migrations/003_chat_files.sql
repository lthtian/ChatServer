-- ABOUTME: Stores generic file metadata alongside image resources and conversation messages.
-- ABOUTME: Keeps image validation requirements separate from opaque file size and metadata constraints.
ALTER TABLE Media
    ADD COLUMN kind ENUM('image','file') NOT NULL DEFAULT 'image',
    ADD COLUMN name VARCHAR(255) NULL,
    DROP CHECK ck_media_size,
    DROP CHECK ck_media_ready,
    ADD CONSTRAINT ck_media_size CHECK (
        (kind='image' AND expected_bytes BETWEEN 1 AND 20971520)
        OR (kind='file' AND expected_bytes<=104857600)),
    ADD CONSTRAINT ck_media_name CHECK (
        kind='image' OR (name IS NOT NULL AND CHAR_LENGTH(name)>0)),
    ADD CONSTRAINT ck_media_ready CHECK (
        state<>'ready' OR (
            actual_bytes IS NOT NULL AND actual_bytes=expected_bytes AND mime IS NOT NULL
            AND ((kind='file' AND mime='application/octet-stream'
                  AND width IS NULL AND height IS NULL AND thumbnail_key IS NULL)
                OR (kind='image' AND mime IN ('image/jpeg','image/png')
                    AND width IS NOT NULL AND width>0 AND height IS NOT NULL AND height>0
                    AND thumbnail_key IS NOT NULL
                    AND thumbnail_mime IS NOT NULL AND thumbnail_mime IN ('image/jpeg','image/png')
                    AND thumbnail_bytes IS NOT NULL AND thumbnail_bytes>0
                    AND thumbnail_width IS NOT NULL AND thumbnail_width BETWEEN 1 AND 320
                    AND thumbnail_height IS NOT NULL AND thumbnail_height BETWEEN 1 AND 320))));
ALTER TABLE History
    MODIFY kind ENUM('text','image','file') NOT NULL DEFAULT 'text',
    DROP CHECK ck_history_media,
    ADD CONSTRAINT ck_history_media CHECK (
        (kind='text' AND media_id IS NULL)
        OR (kind IN ('image','file') AND media_id IS NOT NULL AND client_msg_id IS NOT NULL));
