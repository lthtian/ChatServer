import base64
import hashlib
import json
import os
import pathlib
import signal
import socket
import subprocess
import tempfile
import time
import unittest
import urllib.request
import urllib.error
import uuid

import media_schema_test as schema

PNG = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAADklEQVR4nGPgUbLwA2EABYEBaWcDN6YAAAAASUVORK5CYII=')

def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]

class Client:
    def __init__(self, address):
        self.socket = socket.create_connection(address, timeout=3)
        self.buffer = ''
        self.events = []
    def send(self, payload, ack):
        self.socket.sendall(json.dumps(payload).encode())
        while True:
            while self.buffer:
                try:
                    result, end = json.JSONDecoder().raw_decode(self.buffer)
                except json.JSONDecodeError:
                    break
                self.buffer = self.buffer[end:]
                if result.get('msgid') == ack:
                    return result
                self.events.append(result)
            data = self.socket.recv(65536)
            if not data:
                raise RuntimeError('disconnected')
            self.buffer += data.decode('utf-8')
    def request(self, op, **fields):
        identifier = str(uuid.uuid4())
        result = self.send(dict(msgid=26, request_id=identifier, op=op, **fields), 27)
        assert result['request_id'] == identifier
        return result

def transfer(descriptor, data=None):
    request = urllib.request.Request(descriptor['url'], data=data,
        headers=descriptor['headers'], method=descriptor['method'])
    with urllib.request.urlopen(request, timeout=15) as response:
        return response.read()

class MediaServiceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        schema.ARGS.repository_test = pathlib.Path(os.environ['CHAT_SERVER_TEST_BINARY'])
        schema.MediaSchemaTest.setUpClass()
        cls.addClassCleanup(schema.MediaSchemaTest.doClassCleanups)
        fixture = schema.MediaSchemaTest
        schema.execute('CREATE TABLE NewMsgCnt (`key` VARCHAR(100) PRIMARY KEY, cnt INT);'
                       'CREATE TABLE OfflineMessage(userid INT, message TEXT);'
                       'CREATE TABLE images(id INT PRIMARY KEY, image_data LONGBLOB);'
                       "INSERT INTO User(id,name,password) VALUES(4,'client_sender','test');"
                       "INSERT INTO User(id,name,password) VALUES(5,'ack_sender','test');"
                       'INSERT INTO Friend VALUES(4,2),(2,4),(5,2),(2,5);', fixture.database)
        cls.directory = tempfile.TemporaryDirectory(prefix='chat-media-service-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.control_port, cls.http_port, redis_port = port(), port(), port()
        cls.log = open(pathlib.Path(cls.directory.name) / 'server.log', 'w+')
        cls.addClassCleanup(cls.log.close)
        redis = subprocess.Popen(['redis-server', '--port', str(redis_port), '--save', '',
            '--appendonly', 'no'], stdout=cls.log, stderr=cls.log)
        cls.addClassCleanup(lambda: (redis.terminate(), redis.wait(timeout=10)))
        environment = dict(os.environ, CHAT_DB_NAME=fixture.database, CHAT_DB_USER=fixture.test_user,
            CHAT_DB_PASSWORD=fixture.test_password, CHAT_REDIS_PORT=str(redis_port),
            CHAT_MEDIA_ROOT=cls.directory.name + '/objects', CHAT_MEDIA_PORT=str(cls.http_port),
            CHAT_MEDIA_URL=f'http://127.0.0.1:{cls.http_port}')
        if os.environ.get('CHAT_TEST_DEFAULT_REDIS'):
            environment.pop('CHAT_REDIS_PORT', None)
        cls.environment = environment
        cls.server = subprocess.Popen([os.environ['CHAT_SERVER_TEST_BINARY'], '127.0.0.1',
            str(cls.control_port)], env=environment, stdout=cls.log, stderr=cls.log)
        cls.addClassCleanup(cls.stop)
        for attempt in range(100):
            cls.log.flush()
            cls.log.seek(0)
            if 'ChatServer started' in cls.log.read():
                break
            if cls.server.poll() is not None:
                cls.log.seek(0)
                raise RuntimeError('server exited during startup:\n' + cls.log.read())
            time.sleep(.1)
        else:
            cls.log.flush()
            cls.log.seek(0)
            raise RuntimeError('server startup timeout:\n' + cls.log.read())
        cls.clients = [Client(('127.0.0.1', cls.control_port)) for _ in range(4)]
        for client in cls.clients:
            cls.addClassCleanup(client.socket.close)
        for client, name in zip(cls.clients, ['sender', 'receiver', 'outsider']):
            result = client.send(dict(msgid=1, username=name, password='test'), 2)
            assert result['errno'] == 0, result
    @classmethod
    def stop(cls):
        cls.server.send_signal(signal.SIGINT)
        cls.server.wait(timeout=15)
        cls.log.flush()
        cls.log.seek(0)
        output = cls.log.read()
        if cls.server.returncode != 0 or any(marker in output for marker in
            ['[ERROR]', 'Handler error:', 'Object cleanup deferred', 'terminate called']):
            raise RuntimeError('Server diagnostics were not clean:\n' + output)
    def begin(self, data=PNG, **extra):
        fields = dict(conversation=dict(is_group=False, target=2), client_msg_id=str(uuid.uuid4()),
            bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        fields.update(extra)
        result = self.clients[0].request('begin', **fields)
        self.assertTrue(result['ok'], result)
        return fields, result['data']
    def test_flow(self):
        sender, receiver, outsider, anonymous = self.clients
        self.assertEqual(anonymous.request('history', conversation=dict(is_group=False, target=1))['error'], 'unauthorized')
        malformed = sender.send(dict(msgid=26, request_id=42, op='status'), 27)
        self.assertEqual(malformed['error'], 'invalid_argument')
        fields, media = self.begin()
        self.assertEqual(transfer(media['upload'], PNG), b'{"ok":true}')
        result = sender.request('publish', conversation=fields['conversation'],
            media_id=media['media_id'], client_msg_id=fields['client_msg_id'])
        self.assertTrue(result['ok'], result)
        repeated = sender.request('publish', conversation=fields['conversation'],
            media_id=media['media_id'], client_msg_id=fields['client_msg_id'])
        self.assertEqual(result['data']['message'], repeated['data']['message'])
        self.assertFalse(repeated['data']['created'])
        for variant in ['original', 'thumbnail']:
            read = receiver.request('read', conversation=dict(is_group=False, target=1),
                media_id=media['media_id'], variant=variant)
            self.assertTrue(read['ok'], read)
            data = transfer(read['data']['download'])
            self.assertTrue(data.startswith(b'\x89PNG'))
            if variant == 'original': self.assertEqual(data, PNG)
        denied = outsider.request('read', conversation=dict(is_group=False, target=1),
            media_id=media['media_id'], variant='original')
        self.assertFalse(denied['ok'])
        history = receiver.request('history', conversation=dict(is_group=False, target=1), limit=1)
        self.assertEqual(history['data']['messages'][0]['media']['media_id'], media['media_id'])
        self.assertTrue(history['data']['next_cursor'])
        self.assertEqual(sender.request('cancel', media_id=media['media_id'])['error'], 'already_sent')
    def test_byte_ranges(self):
        # 使用真实服务、磁盘和 HTTP，覆盖跨越 64 KiB 缓冲边界的读取。
        content = bytes(range(256)) * 1024 + b'end'
        sender, receiver = self.clients[:2]
        conversation = dict(is_group=False, target=2)
        message_id = str(uuid.uuid4())
        media = sender.request('begin_file', conversation=conversation,
            client_msg_id=message_id, bytes=len(content),
            sha256=hashlib.sha256(content).hexdigest(), name='range-check.mp4')['data']
        transfer(media['upload'], content)
        self.assertTrue(sender.request('publish', conversation=conversation,
            media_id=media['media_id'], client_msg_id=message_id)['ok'])
        descriptor = receiver.request('read', conversation=dict(is_group=False, target=1),
            media_id=media['media_id'], variant='original')['data']['download']
        cases = [('', 200, 0, len(content)), ('bytes=0-0', 206, 0, 1),
            ('bytes=65530-131080', 206, 65530, 131081),
            ('bytes=260000-', 206, 260000, len(content)),
            ('bytes=-3', 206, len(content) - 3, len(content)),
            ('bytes=-999999', 206, 0, len(content)),
            ('bytes=262144-999999', 206, 262144, len(content)),
            ('bytes=0-1,4-5', 200, 0, len(content))]
        for value, status, begin, end in cases:
            with self.subTest(range=value):
                headers = dict(descriptor['headers'])
                if value: headers['Range'] = value
                request = urllib.request.Request(descriptor['url'], headers=headers)
                with urllib.request.urlopen(request, timeout=10) as response:
                    self.assertEqual(response.status, status)
                    self.assertEqual(response.headers['Accept-Ranges'], 'bytes')
                    self.assertEqual(int(response.headers['Content-Length']), end - begin)
                    self.assertEqual(response.read(), content[begin:end])
                    if status == 206:
                        self.assertEqual(response.headers['Content-Range'],
                            f'bytes {begin}-{end-1}/{len(content)}')
        for value, status in [(f'bytes={len(content)}-', 416), ('bytes=-0', 416),
            ('bytes=8-2', 400), ('bytes=abc-', 400),
            ('bytes=18446744073709551616-', 400)]:
            headers = dict(descriptor['headers'], Range=value)
            with self.assertRaises(urllib.error.HTTPError) as failed:
                urllib.request.urlopen(urllib.request.Request(descriptor['url'], headers=headers), timeout=10)
            self.assertEqual(failed.exception.code, status)
            if status == 416:
                self.assertEqual(failed.exception.headers['Content-Range'], f'bytes */{len(content)}')
                self.assertEqual(failed.exception.read(), b'')
            else:
                self.assertEqual(json.loads(failed.exception.read())['error'], 'invalid_range')

    def test_bad_hash_and_retry(self):
        fields, media = self.begin(sha256='0' * 64)
        with self.assertRaises(urllib.error.HTTPError) as failed:
            transfer(media['upload'], PNG)
        self.assertEqual(json.loads(failed.exception.read())['error'], 'hash_mismatch')
        state = self.clients[0].request('status', media_id=media['media_id'])
        self.assertEqual(state['data']['state'], 'failed')
        self.assertTrue(self.clients[0].request('cancel', media_id=media['media_id'])['ok'])

    def test_interrupted_upload_group_and_retry(self):
        fields, media = self.begin(conversation=dict(is_group=True, target=7))
        from urllib.parse import urlparse
        endpoint = urlparse(media['upload']['url'])
        stream = socket.create_connection((endpoint.hostname, endpoint.port))
        header = (f'PUT /media HTTP/1.1\r\nHost: localhost\r\nContent-Length: {len(PNG)}\r\n'
                  f'Authorization: {media["upload"]["headers"]["Authorization"]}\r\n\r\n')
        stream.sendall(header.encode() + PNG[:10])
        stream.close()
        for attempt in range(30):
            state = self.clients[0].request('status', media_id=media['media_id'])['data']
            if state['state'] == 'failed': break
            time.sleep(.1)
        self.assertEqual(state['state'], 'failed')
        retried = self.clients[0].request('retry', media_id=media['media_id'])['data']
        self.assertEqual(transfer(retried['upload'], PNG), b'{"ok":true}')
        sent = self.clients[0].request('publish', conversation=fields['conversation'],
            media_id=media['media_id'], client_msg_id=fields['client_msg_id'])
        self.assertTrue(sent['ok'], sent)
        accessible = self.clients[1].request('read', conversation=dict(is_group=True, target=7),
            media_id=media['media_id'], variant='original')
        self.assertTrue(accessible['ok'], accessible)
        denied = self.clients[2].request('read', conversation=dict(is_group=True, target=7),
            media_id=media['media_id'], variant='original')
        self.assertFalse(denied['ok'])

    def test_expired_upload_can_be_retried(self):
        fields, media = self.begin()
        schema.execute("UPDATE Media SET expires_at=CURRENT_TIMESTAMP-INTERVAL 1 SECOND WHERE media_id='"
                       + media['media_id'] + "'", schema.MediaSchemaTest.database)
        retried = self.clients[0].request('retry', media_id=media['media_id'])
        self.assertTrue(retried['ok'], retried)
        self.assertEqual(retried['data']['media_id'], media['media_id'])
        transfer(retried['data']['upload'], PNG)
        self.assertTrue(self.clients[0].request('publish', conversation=fields['conversation'],
            media_id=media['media_id'], client_msg_id=fields['client_msg_id'])['ok'])
    def test_cancel_and_size(self):
        _, media = self.begin()
        self.assertTrue(self.clients[0].request('cancel', media_id=media['media_id'])['ok'])
        with self.assertRaises(urllib.error.HTTPError):
            transfer(media['upload'], PNG)
        invalid = self.clients[0].request('begin', conversation=dict(is_group=False, target=2),
            client_msg_id=str(uuid.uuid4()), bytes=20971521, sha256='a' * 64)
        self.assertFalse(invalid['ok'])

    def test_lost_ack_does_not_duplicate_message(self):
        address = ('127.0.0.1', self.control_port)
        sender = Client(address)
        self.addCleanup(sender.socket.close)
        self.assertEqual(sender.send(dict(msgid=1, username='ack_sender', password='test'), 2)['errno'], 0)
        target = dict(is_group=False, target=2)
        client_id = str(uuid.uuid4())
        media = sender.request('begin', conversation=target, client_msg_id=client_id,
            bytes=len(PNG), sha256=hashlib.sha256(PNG).hexdigest())['data']
        transfer(media['upload'], PNG)
        sender.socket.sendall(json.dumps(dict(msgid=26, request_id=str(uuid.uuid4()), op='publish',
            conversation=target, media_id=media['media_id'], client_msg_id=client_id)).encode())
        for attempt in range(30):
            count = schema.execute("SELECT COUNT(*) FROM History WHERE client_msg_id='" + client_id + "'",
                schema.MediaSchemaTest.database).stdout.strip()
            if count == '1': break
            time.sleep(.1)
        self.assertEqual(count, '1')
        sender.socket.close()
        for attempt in range(30):
            state = schema.execute('SELECT state FROM User WHERE id=5', schema.MediaSchemaTest.database).stdout.strip()
            if state == 'offline': break
            time.sleep(.1)
        retry = Client(address); self.addCleanup(retry.socket.close)
        self.assertEqual(retry.send(dict(msgid=1, username='ack_sender', password='test'), 2)['errno'], 0)
        result = retry.request('publish', conversation=target, media_id=media['media_id'], client_msg_id=client_id)
        self.assertTrue(result['ok'], result)
        self.assertFalse(result['data']['created'])
        self.assertEqual(schema.execute("SELECT COUNT(*) FROM History WHERE client_msg_id='" + client_id + "'",
            schema.MediaSchemaTest.database).stdout.strip(), '1')

    @unittest.skipUnless(os.environ.get('CHAT_HLS_BUNDLE') and os.environ.get('CHAT_HLS_SOURCE'), 'HLS fixtures not selected')
    def test_hls_assets(self):
        import shutil
        folder = pathlib.Path(os.environ['CHAT_HLS_BUNDLE'])
        sample = pathlib.Path(os.environ['CHAT_HLS_SOURCE']).read_bytes()
        sender, receiver, outsider, anonymous = self.clients
        conversation = dict(is_group=False, target=2)
        intent = str(uuid.uuid4())
        begin = sender.request('begin_file', conversation=conversation, client_msg_id=intent,
            bytes=len(sample), sha256=hashlib.sha256(sample).hexdigest(), name='hls_sample.mp4')
        self.assertTrue(begin['ok'], begin)
        media = begin['data']
        self.assertEqual(json.loads(transfer(media['upload'], sample)), {'ok': True})
        self.assertTrue(sender.request('publish', conversation=conversation,
            client_msg_id=intent, media_id=media['media_id'])['ok'])
        fields = dict(conversation=dict(is_group=False, target=1), media_id=media['media_id'], variant='playback')
        before = receiver.request('read', **fields)
        self.assertEqual(before['data']['format'], 'mp4')
        catalog = json.loads((folder / 'catalog.json').read_text())
        base = pathlib.Path(self.directory.name) / 'objects' / 'objects' / media['media_id'] / 'hls'
        base.mkdir(parents=True)
        (base / 'status').write_text('{"state":"processing"}')
        self.assertEqual(receiver.request('read', **fields)['data']['hls_state'], 'processing')
        for asset in catalog['assets']:
            target = base / catalog['revision'] / asset['path'].replace('.', '_')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(folder / asset['path'], target)
        (base / 'catalog').write_text(json.dumps(catalog))
        ready = receiver.request('read', **fields)
        self.assertTrue(ready['ok'], ready)
        self.assertEqual(ready['data']['format'], 'hls')
        self.assertEqual([v['id'] for v in ready['data']['variants']], [v['id'] for v in catalog['variants']])
        descriptor = ready['data']['download']
        for asset in catalog['assets']:
            item = dict(descriptor, url=descriptor['url'].rsplit('/', 1)[0] + '/' + asset['path'])
            self.assertEqual(hashlib.sha256(transfer(item)).hexdigest(), asset['sha256'])
        self.assertFalse(outsider.request('read', **fields)['ok'])
        self.assertFalse(anonymous.request('read', **fields)['ok'])
        for url, headers in [(descriptor['url'], {}),
                (descriptor['url'].replace(catalog['revision'], '0' * 32), descriptor['headers']),
                (descriptor['url'].replace('master.m3u8', '../original'), descriptor['headers']),
                (descriptor['url'], before['data']['download']['headers'])]:
            with self.assertRaises(urllib.error.HTTPError) as failed:
                transfer(dict(descriptor, url=url, headers=headers))
            self.assertEqual(json.loads(failed.exception.read())['error'], 'unauthorized')
        # 完整原件下载仍独立于 HLS 发布状态。
        original = receiver.request('read', **dict(fields, variant='original'))
        self.assertEqual(transfer(original['data']['download']), sample)

    @unittest.skipUnless(os.environ.get('CHAT_WINDOWS_CLIENT_TEST'), 'Windows client integration not selected')
    def test_windows_client(self):
        command = [
            '/mnt/c/Users/lds/scoop/apps/nodejs/current/node.exe',
            '--test-reporter=tap',
            'E:\\chat_server_qt\\ChatClient\\chat_client_electron\\tests\\image_client.test.js',
            str(self.control_port)]
        try:
            result = subprocess.run(command, stdin=subprocess.DEVNULL,
                capture_output=True, text=True, encoding='utf-8', timeout=90,
                cwd='/mnt/e/chat_server_qt/ChatClient/chat_client_electron')
        except subprocess.TimeoutExpired as error:
            raise RuntimeError('Windows client timed out: ' + repr(error.stdout) + repr(error.stderr)) from error
        self.assertEqual(result.stderr, '', result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn('# fail 0', result.stdout)

if __name__ == '__main__':
    if os.environ.get('CHAT_FIXTURE_FILE'):
        try:
            MediaServiceTest.setUpClass()
            pathlib.Path(os.environ['CHAT_FIXTURE_FILE']).write_text(json.dumps({
                'port': MediaServiceTest.control_port, 'http_port': MediaServiceTest.http_port}))
            print('READY: press Enter to close the isolated fixture', flush=True)
            input()
        finally:
            MediaServiceTest.doClassCleanups()
    else:
        unittest.main(argv=[__file__] + schema.TEST_ARGS)
