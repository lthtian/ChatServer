# ABOUTME: Verifies generic file publication and downloads against the actual chat service.
# ABOUTME: Uses isolated MySQL records and real HTTP bytes to test metadata and access control.
import hashlib
import uuid
import unittest
import media_service_test as media


class FileServiceTest(media.MediaServiceTest):
    def test_file_roundtrip_and_history(self):
        data = b'\x00ordinary file\xff\r\n'
        target = dict(is_group=False, target=2)
        identifier = str(uuid.uuid4())
        fields = dict(conversation=target, client_msg_id=identifier,
                      bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), name='资料 2026.bin')
        begun = self.clients[0].request('begin_file', **fields)
        self.assertTrue(begun['ok'], begun)
        resource = begun['data']
        self.assertEqual(media.transfer(resource['upload'], data), b'{"ok":true}')
        published = self.clients[0].request('publish', conversation=target,
            client_msg_id=identifier, media_id=resource['media_id'])
        self.assertTrue(published['ok'], published)
        message = published['data']['message']
        self.assertEqual(message['kind'], 'file')
        self.assertEqual(message['media']['name'], fields['name'])
        self.assertEqual(message['media']['bytes'], len(data))
        self.assertGreater(int(message['sequence']), 0)
        again = self.clients[0].request('publish', conversation=target,
            client_msg_id=identifier, media_id=resource['media_id'])
        self.assertFalse(again['data']['created'])
        self.assertEqual(again['data']['message'], message)
        receiver = dict(is_group=False, target=1)
        download = self.clients[1].request('read', conversation=receiver,
            media_id=resource['media_id'], variant='original')
        self.assertTrue(download['ok'], download)
        self.assertEqual(media.transfer(download['data']['download']), data)
        self.assertEqual(download['data']['mime'], 'application/octet-stream')
        self.assertEqual(download['data']['sha256'], fields['sha256'])
        self.assertFalse(self.clients[1].request('read', conversation=receiver,
            media_id=resource['media_id'], variant='thumbnail')['ok'])
        self.assertFalse(self.clients[2].request('read', conversation=receiver,
            media_id=resource['media_id'], variant='original')['ok'])
        history = self.clients[1].request('sync', conversation=receiver, direction='initial')
        self.assertIn(message, history['data']['messages'])
        fields['name'] = 'different.bin'
        self.assertEqual(self.clients[0].request('begin_file', **fields)['error'], 'conflict')

    def test_file_name_validation(self):
        for name in ('../outside.txt', 'C:\\outside.txt', 'bad\x00name', ''):
            with self.subTest(name=name):
                result = self.clients[0].request('begin_file', conversation=dict(is_group=False, target=2),
                    client_msg_id=str(uuid.uuid4()), bytes=1,
                    sha256=hashlib.sha256(b'x').hexdigest(), name=name)
                self.assertEqual(result.get('error'), 'invalid_argument')

    def test_empty_file_and_size_limit(self):
        target = dict(is_group=True, target=7)
        identifier = str(uuid.uuid4())
        fields = dict(conversation=target, client_msg_id=identifier,
                      bytes=0, sha256=hashlib.sha256(b'').hexdigest(), name='empty.txt')
        result = self.clients[0].request('begin_file', **fields)
        self.assertTrue(result['ok'], result)
        self.assertEqual(media.transfer(result['data']['upload'], b''), b'{"ok":true}')
        sent = self.clients[0].request('publish', conversation=target,
            client_msg_id=identifier, media_id=result['data']['media_id'])
        self.assertTrue(sent['ok'], sent)
        download = self.clients[1].request('read', conversation=target,
            media_id=result['data']['media_id'], variant='original')
        self.assertEqual(media.transfer(download['data']['download']), b'')
        fields.update(client_msg_id=str(uuid.uuid4()), bytes=104857601)
        self.assertEqual(self.clients[0].request('begin_file', **fields)['error'], 'invalid_argument')


if __name__ == '__main__':
    unittest.main(argv=[__file__] + media.schema.TEST_ARGS)
