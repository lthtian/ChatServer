"""真实 MySQL、HTTP 上传和媒体处理进程；只运行本文件指定的必要用例。"""
import hashlib
import json
import os
import pathlib
import subprocess
import time
import unittest
import uuid

from media_service_test import Client, MediaServiceTest, transfer
import media_schema_test as schema


class MediaJobsTest(MediaServiceTest):
    def upload_video(self, content, name):
        client_id = str(uuid.uuid4())
        conversation = dict(is_group=False, target=2)
        result = self.clients[0].request('begin_file', conversation=conversation,
            client_msg_id=client_id, bytes=len(content), sha256=hashlib.sha256(content).hexdigest(), name=name)
        self.assertTrue(result['ok'], result)
        media = result['data']
        self.assertEqual(json.loads(transfer(media['upload'], content)), {'ok': True})
        self.assertTrue(self.clients[0].request('publish', conversation=conversation,
            client_msg_id=client_id, media_id=media['media_id'])['ok'])
        return media['media_id']

    def wait_state(self, media_id, states, timeout=120):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = self.clients[0].request('status', media_id=media_id)
            self.assertTrue(result['ok'], result)
            job = result['data']['hls_job']
            if job.get('state') in states:
                return job
            time.sleep(.05)
        self.fail(f'timed out waiting for {states}: {job}')

    def test_automatic_processing(self):
        content = pathlib.Path(os.environ['CHAT_JOB_SAMPLE']).read_bytes()
        media_id = self.upload_video(content, 'automatic.mp4')
        job = self.wait_state(media_id, {'ready', 'failed'})
        self.assertEqual(job['state'], 'ready', job)
        self.assertEqual(job['attempts'], 1)
        fields = dict(conversation=dict(is_group=False, target=1), media_id=media_id)
        result = self.clients[1].request('read', variant='playback', **fields)
        self.assertTrue(result['ok'], result)
        self.assertEqual(result['data']['format'], 'hls')
        root = pathlib.Path(self.directory.name) / 'objects' / 'objects' / media_id
        catalog = json.loads((root / 'hls' / 'catalog').read_text())
        self.assertEqual(result['data']['variants'][0]['height'], 480)
        self.assertEqual([x['height'] for x in catalog['variants']], [480, 360])
        descriptor = result['data']['download']
        for asset in catalog['assets']:
            url = descriptor['url'].rsplit('/', 1)[0] + '/' + asset['path']
            self.assertEqual(hashlib.sha256(transfer(dict(descriptor, url=url))).hexdigest(), asset['sha256'])
        self.assertEqual(transfer(self.clients[1].request('read', variant='original', **fields)['data']['download']), content)
        self.assertFalse(self.clients[2].request('read', variant='playback', **fields)['ok'])
        # 重复请求播放只读取产物，不重新转码。
        self.clients[1].request('read', variant='playback', **fields)
        self.assertEqual(self.clients[0].request('status', media_id=media_id)['data']['hls_job']['attempts'], 1)

    def test_failure_and_retry(self):
        content = b'not an MP4 file'
        media_id = self.upload_video(content, 'invalid.mp4')
        failed = self.wait_state(media_id, {'failed'})
        self.assertEqual(failed['failure_code'], 'transcode_failed')
        fields = dict(conversation=dict(is_group=False, target=1), media_id=media_id)
        response = self.clients[1].request('read', variant='playback', **fields)['data']
        self.assertEqual(response['hls_state'], 'failed')
        self.assertEqual(response['format'], 'mp4')
        self.assertEqual(transfer(self.clients[1].request('read', variant='original', **fields)['data']['download']), content)
        self.assertFalse(self.clients[1].request('retry_hls', media_id=media_id)['ok'])
        self.assertTrue(self.clients[0].request('retry_hls', media_id=media_id)['ok'])
        self.wait_state(media_id, {'failed'})
        log = pathlib.Path(self.directory.name) / 'objects' / 'objects' / media_id / 'processing' / 'processor.log'
        diagnostic = log.read_text()
        self.assertIn('avformat_open_input', diagnostic)
        self.assertIn('Invalid data found when processing input', diagnostic)
        self.assertFalse((pathlib.Path(self.directory.name) / 'objects' / 'objects' / media_id / 'hls' / 'catalog').exists())

    def test_restart_recovers_processing(self):
        content = pathlib.Path(os.environ['CHAT_JOB_SAMPLE']).read_bytes()
        media_id = self.upload_video(content, 'restart.mp4')
        self.wait_state(media_id, {'processing'})
        cls = type(self)
        cls.stop()
        cls.server = subprocess.Popen([os.environ['CHAT_SERVER_TEST_BINARY'], '127.0.0.1',
            str(cls.control_port)], env=cls.environment, stdout=cls.log, stderr=cls.log)
        for attempt in range(100):
            try:
                clients = [Client(('127.0.0.1', cls.control_port)) for _ in range(4)]
                break
            except OSError:
                self.assertIsNone(cls.server.poll())
                time.sleep(.1)
        else:
            self.fail('server did not restart')
        for client in cls.clients:
            client.socket.close()
        cls.clients = clients
        for client in clients:
            cls.addClassCleanup(client.socket.close)
        for client, name in zip(clients, ['sender', 'receiver', 'outsider']):
            self.assertEqual(client.send(dict(msgid=1, username=name, password='test'), 2)['errno'], 0)
        job = self.wait_state(media_id, {'ready', 'failed'})
        self.assertEqual(job['state'], 'ready', job)
        self.assertEqual(job['attempts'], 2)


if __name__ == '__main__':
    suite = unittest.TestSuite(MediaJobsTest(name) for name in (
        'test_automatic_processing', 'test_failure_and_retry', 'test_restart_recovers_processing'))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
