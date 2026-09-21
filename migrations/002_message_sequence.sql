-- ABOUTME: Assigns transaction-ordered sequence numbers within each conversation.
-- ABOUTME: Backfills existing messages before enabling sequence allocation for every insert.

-- Apply while message writers are stopped; MySQL DDL is not one atomic transaction.
ALTER TABLE History ADD COLUMN conversation_seq BIGINT NULL;
CREATE TEMPORARY TABLE HistorySequence AS
    SELECT id, ROW_NUMBER() OVER (PARTITION BY isgroup, chatkey ORDER BY id) AS seq FROM History;
UPDATE History h JOIN HistorySequence s ON s.id=h.id SET h.conversation_seq=s.seq;
DROP TEMPORARY TABLE HistorySequence;
ALTER TABLE History MODIFY conversation_seq BIGINT NOT NULL DEFAULT 0,
    ADD UNIQUE KEY uq_history_sequence (isgroup, chatkey, conversation_seq);

CREATE TABLE ConversationSequence (
    isgroup TINYINT(1) NOT NULL,
    chatkey VARCHAR(50) COLLATE utf8mb4_unicode_ci NOT NULL,
    last_sequence BIGINT NOT NULL,
    PRIMARY KEY (isgroup, chatkey)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
INSERT INTO ConversationSequence
    SELECT isgroup, chatkey, MAX(conversation_seq) FROM History GROUP BY isgroup, chatkey;

DELIMITER //
CREATE TRIGGER history_sequence BEFORE INSERT ON History FOR EACH ROW
BEGIN
    INSERT INTO ConversationSequence VALUES(NEW.isgroup, NEW.chatkey, 0)
        ON DUPLICATE KEY UPDATE last_sequence=last_sequence;
    UPDATE ConversationSequence SET last_sequence=last_sequence+1
        WHERE isgroup=NEW.isgroup AND chatkey=NEW.chatkey;
    SET NEW.conversation_seq=(SELECT last_sequence FROM ConversationSequence
        WHERE isgroup=NEW.isgroup AND chatkey=NEW.chatkey);
END//
DELIMITER ;
