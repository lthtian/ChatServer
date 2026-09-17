# ABOUTME: Validates media schema and message constraints in an isolated MySQL database.
# ABOUTME: Creates a uniquely named test database and removes only that database afterward.
import argparse
import os
import pathlib
import secrets
import subprocess
import unittest

BASE = pathlib.Path(__file__).resolve().parents[1]
OPTIONS = argparse.ArgumentParser()
OPTIONS.add_argument('--sudo', action='store_true')
OPTIONS.add_argument('--repository-test', type=pathlib.Path)
ARGS, TEST_ARGS = OPTIONS.parse_known_args()
MYSQL = (['sudo', '-n'] if ARGS.sudo else []) + [
    'mysql', '--batch', '--raw', '--skip-column-names']


def execute(sql, database=None, expect_success=True):
    command = MYSQL + ([database] if database else [])
    result = subprocess.run(command, input=sql, capture_output=True,
                            encoding='utf-8', timeout=30)
    if expect_success and result.returncode:
        raise RuntimeError(result.stderr.strip())
    return result


class MediaSchemaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.database = 'chat_media_test_' + secrets.token_hex(6)
        execute('CREATE DATABASE `' + cls.database + '` CHARACTER SET utf8mb4 '
                'COLLATE utf8mb4_unicode_ci;')
        cls.addClassCleanup(lambda: execute('DROP DATABASE `' + cls.database + '`;'))
        execute('''
            CREATE TABLE User (
                id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
                name VARCHAR(50) NOT NULL UNIQUE,
                password VARCHAR(100) NOT NULL,
                state ENUM('online','offline') DEFAULT 'offline'
            ) ENGINE=InnoDB;
            CREATE TABLE History (
                id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
                chatkey VARCHAR(50) NOT NULL,
                userid INT NOT NULL,
                isgroup TINYINT(1) NOT NULL DEFAULT 0,
                message TEXT NOT NULL,
                time TIMESTAMP NULL DEFAULT CURRENT_TIMESTAMP,
                KEY userid (userid), KEY idx_history_chatkey (chatkey),
                KEY idx_history_time (time),
                CONSTRAINT History_ibfk_1 FOREIGN KEY(userid) REFERENCES User(id)
                    ON DELETE CASCADE
            ) ENGINE=InnoDB;
            INSERT INTO User(id,name,password) VALUES(1,'sender','test'),(2,'receiver','test');
            INSERT INTO History(chatkey,userid,message) VALUES('1#2',1,'text fixture');
            INSERT INTO User(id,name,password) VALUES(3,'outsider','test');
            CREATE TABLE Friend (
                userid INT NOT NULL, friendid INT NOT NULL,
                PRIMARY KEY(userid,friendid),
                FOREIGN KEY(userid) REFERENCES User(id) ON DELETE CASCADE,
                FOREIGN KEY(friendid) REFERENCES User(id) ON DELETE CASCADE
            ) ENGINE=InnoDB;
            CREATE TABLE AllGroup (
                id INT NOT NULL PRIMARY KEY, groupname VARCHAR(50) NOT NULL
            ) ENGINE=InnoDB;
            CREATE TABLE GroupUser (
                userid INT NOT NULL, groupid INT NOT NULL,
                grouprole ENUM('creator','normal') DEFAULT 'normal',
                PRIMARY KEY(userid,groupid),
                FOREIGN KEY(userid) REFERENCES User(id) ON DELETE CASCADE,
                FOREIGN KEY(groupid) REFERENCES AllGroup(id) ON DELETE CASCADE
            ) ENGINE=InnoDB;
            INSERT INTO Friend VALUES(1,2),(2,1);
            INSERT INTO AllGroup VALUES(7,'test group');
            INSERT INTO GroupUser VALUES(1,7,'creator'),(2,7,'normal');
        ''', cls.database)
        execute((BASE / 'migrations' / '001_chat_media.sql').read_text(encoding='utf-8'),
                cls.database)
        if ARGS.repository_test:
            cls.test_user = 'chat_test_' + secrets.token_hex(6)
            cls.test_password = secrets.token_hex(24)
            execute("CREATE USER '" + cls.test_user + "'@'localhost' "
                    "IDENTIFIED WITH mysql_native_password BY '" + cls.test_password + "';")
            cls.addClassCleanup(lambda: execute("DROP USER '" + cls.test_user + "'@'localhost';"))
            execute('GRANT SELECT,INSERT,UPDATE,DELETE ON `' + cls.database
                    + "`.* TO '" + cls.test_user + "'@'localhost';")

    def test_text_row(self):
        result = execute('SELECT kind, media_id IS NULL FROM History WHERE id=1;',
                         self.database)
        self.assertEqual(result.stdout.strip(), 'text\t1')

    def test_sender_idempotency_constraint(self):
        identifier = '00000000-0000-4000-8000-000000000099'
        sql = ("INSERT INTO History(chatkey,userid,message,client_msg_id) "
               "VALUES('1#2',1,'idempotency fixture','" + identifier + "');")
        execute(sql, self.database)
        duplicate = execute(sql, self.database, expect_success=False)
        self.assertNotEqual(duplicate.returncode, 0)
        self.assertIn('1062', duplicate.stderr)
        self.assertEqual(duplicate.stdout, '')
        count = execute("SELECT COUNT(*) FROM History WHERE client_msg_id='"
                        + identifier + "';", self.database)
        self.assertEqual(count.stdout.strip(), '1')

    def test_image_requires_media(self):
        result = execute("INSERT INTO History(chatkey,userid,message,kind) "
                         "VALUES('1#2',1,'','image');", self.database,
                         expect_success=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('3819', result.stderr)

    def test_media_table(self):
        columns = execute('SHOW COLUMNS FROM Media;', self.database)
        names = {line.split('\t')[0] for line in columns.stdout.splitlines()}
        self.assertTrue({'media_id', 'owner_id', 'state', 'provider', 'original_key',
                         'thumbnail_key', 'expected_bytes', 'actual_bytes',
                         'client_msg_id', 'expires_at'}.issubset(names))


def repository_cases(self):
    environment = dict(os.environ, CHAT_TEST_DATABASE=self.database,
                       CHAT_TEST_USER=self.test_user, CHAT_TEST_PASSWORD=self.test_password)
    for scenario in ('lifecycle', 'permissions', 'cancel', 'failure', 'expiry', 'group', 'prepare'):
        with self.subTest(scenario=scenario):
            result = subprocess.run([str(ARGS.repository_test.resolve()), scenario],
                                    env=environment, capture_output=True,
                                    encoding='utf-8', timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stderr, '')
            self.assertEqual(result.stdout.strip(), 'PASS ' + scenario)
    processes = [subprocess.Popen([str(ARGS.repository_test.resolve()), 'send'],
                                  env=environment, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, encoding='utf-8') for _ in range(8)]
    outputs = [process.communicate(timeout=30) for process in processes]
    for process, (output, error) in zip(processes, outputs):
        self.assertEqual(process.returncode, 0, output + error)
        self.assertEqual(error, '')
        self.assertEqual(output.strip(), 'PASS send')
    count = execute("SELECT COUNT(*) FROM History WHERE client_msg_id="
                    "'00000000-0000-4000-8000-000000000004';", self.database)
    self.assertEqual(count.stdout.strip(), '1', 'Concurrent send inserted duplicate messages')


if __name__ == '__main__':
    if ARGS.repository_test:
        MediaSchemaTest.test_repository = repository_cases
    unittest.main(argv=['media_schema_test.py'] + TEST_ARGS)
