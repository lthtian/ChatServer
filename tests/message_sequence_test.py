# ABOUTME: Validates conversation ordering and rollback using an isolated real MySQL database.
# ABOUTME: Exercises concurrent writers and migration backfill independently of the server binary.
import subprocess
import unittest
import media_schema_test as schema


class MessageSequenceTest(schema.MediaSchemaTest):
    def test_sequence_rollback_and_concurrency(self):
        database = self.database
        result = schema.execute('SELECT conversation_seq FROM History WHERE id=1', database)
        self.assertEqual(result.stdout.strip(), '1')
        schema.execute("START TRANSACTION; INSERT INTO History(chatkey,userid,message) "
                       "VALUES('sequence-test',1,'rollback'); ROLLBACK;", database)
        self.assertEqual(schema.execute("SELECT COUNT(*) FROM ConversationSequence WHERE chatkey='sequence-test'",
                         database).stdout.strip(), '0')
        processes = []
        for number in range(4):
            process = subprocess.Popen(schema.MYSQL + [database], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            process.stdin.write("START TRANSACTION; INSERT INTO History(chatkey,userid,message) "
                "VALUES('sequence-test',1,'parallel'); DO SLEEP(0.1); COMMIT;\n")
            process.stdin.close(); process.stdin = None
            processes.append(process)
        for process in processes:
            output, error = process.communicate(timeout=15)
            self.assertEqual(process.returncode, 0, error)
            self.assertEqual(output, ''); self.assertEqual(error, '')
        result = schema.execute("SELECT conversation_seq FROM History WHERE chatkey='sequence-test' ORDER BY conversation_seq", database)
        self.assertEqual(result.stdout.splitlines(), ['1', '2', '3', '4'])
        self.assertEqual(schema.execute("SELECT last_sequence FROM ConversationSequence WHERE chatkey='sequence-test'",
                         database).stdout.strip(), '4')


if __name__ == '__main__':
    unittest.main(argv=['message_sequence_test.py'] + schema.TEST_ARGS)
