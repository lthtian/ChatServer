"""Real upload, authoritative description, authorization and stored-file contracts."""
import hashlib
import json
import os
from pathlib import Path
import unittest
import uuid

from media_service_test import MediaServiceTest, transfer
import media_schema_test as schema


class MediaCapabilitiesTest(MediaServiceTest):
    def upload_file(self, name, content):
        conversation = dict(is_group=False, target=2)
        client_id = str(uuid.uuid4())
        reply = self.clients[0].request('begin_file', conversation=conversation,
            client_msg_id=client_id, bytes=len(content), sha256=hashlib.sha256(content).hexdigest(), name=name)
        self.assertTrue(reply['ok'], reply)
        item = reply['data']
        self.assertEqual(json.loads(transfer(item['upload'], content)), {'ok': True})
        sent = self.clients[0].request('publish', conversation=conversation,
            client_msg_id=client_id, media_id=item['media_id'])
        self.assertTrue(sent['ok'], sent)
        return item['media_id']

    def read(self, media_id, variant, actor=1):
        return self.clients[actor].request('read', conversation=dict(is_group=False, target=1),
                                          media_id=media_id, variant=variant)

    def test_description_and_original(self):
        samples = Path(os.environ['CHAT_MEDIA_SAMPLES'])
        for name, demuxer, codec in [('video-h265.mkv', 'matroska', 'hevc'),
                                      ('video-vp9-opus.webm', 'matroska', 'vp9'),
                                      ('renamed.data', 'mov', 'h264')]:
            with self.subTest(name=name):
                content = (samples / name).read_bytes()
                media_id = self.upload_file(name, content)
                reply = self.read(media_id, 'description')
                self.assertTrue(reply['ok'], reply)
                description = reply['data']['description']
                self.assertEqual(description['state'], 'recognized')
                self.assertEqual(description['sha256'], hashlib.sha256(content).hexdigest())
                self.assertEqual(description['demuxer'], demuxer)
                selected = next(t for t in description['tracks'] if t['index'] == description['video_stream'])
                self.assertEqual(selected['codec'], codec)
                original = self.read(media_id, 'original')['data']
                self.assertEqual(original['description'], description)
                self.assertEqual(transfer(original['download']), content)
                playback = self.read(media_id, 'playback')['data']
                self.assertIn(playback['format'], ('original', 'hls'))
                self.assertEqual(playback['description'], description)
                if demuxer == 'matroska': self.assertEqual(playback['hls_state'], 'not_applicable')
                self.assertFalse(self.read(media_id, 'description', actor=2)['ok'])
                history = self.clients[1].request('history', conversation=dict(is_group=False, target=1), limit=100)
                self.assertTrue(any(m.get('media', {}).get('media_id') == media_id for m in history['data']['messages']))

    def test_cover_and_corrupt_description(self):
        content = (Path(os.environ['CHAT_MEDIA_SAMPLES']) / 'audio-cover.m4a').read_bytes()
        media_id = self.upload_file('audio-cover.m4a', content)
        reply = self.read(media_id, 'playback')['data']
        self.assertEqual(reply['description']['state'], 'not_video')
        self.assertEqual(reply['format'], 'unavailable')
        stored = Path(self.directory.name) / 'objects' / 'objects' / media_id / 'description'
        description = json.loads(stored.read_text())
        description['sha256'] = '0' * 64
        stored.write_text(json.dumps(description))
        rejected = self.read(media_id, 'playback')['data']
        self.assertEqual(rejected['description']['failure'], 'media_description_invalid')
        self.assertNotIn('download', rejected)
        self.assertEqual(transfer(self.read(media_id, 'original')['data']['download']), content)

    def test_multiple_audio_preserves_original(self):
        for name in ('dual.mp4', 'two-video.mp4'):
            with self.subTest(name=name):
                content = (Path(os.environ['CHAT_TRACK_SAMPLES']) / name).read_bytes()
                media_id = self.upload_file(name, content)
                playback = self.read(media_id, 'playback')['data']
                self.assertEqual(playback['format'], 'original')
                self.assertEqual(playback['hls_state'], 'not_applicable')
                self.assertEqual(playback['variants'], [])
                if name == 'dual.mp4':
                    self.assertEqual(sum(t['type'] == 'audio' for t in playback['description']['tracks']), 2)
                    self.assertEqual(playback['description']['audio_stream'], 2)
                    self.assertEqual(playback['description']['tracks'][2]['language'], 'zho')
                else:
                    self.assertEqual(playback['description']['video_stream'], 1)
                self.assertEqual(transfer(playback['download']), content)
                self.assertEqual(self.clients[0].request('status', media_id=media_id)['data']['hls_job'], {})

    def test_non_video_and_missing_description(self):
        for name, content in [('ordinary.bin', b'ordinary file'), ('invalid.mp4', b'not video')]:
            media_id = self.upload_file(name, content)
            reply = self.read(media_id, 'playback')['data']
            self.assertEqual(reply['description']['state'], 'unrecognized')
            self.assertEqual(reply['format'], 'unavailable')
            self.assertNotIn('download', reply)
            self.assertEqual(transfer(self.read(media_id, 'original')['data']['download']), content)
            self.assertEqual(self.clients[0].request('status', media_id=media_id)['data']['hls_job'], {})
            stored = Path(self.directory.name) / 'objects' / 'objects' / media_id / 'description'
            self.assertTrue(stored.is_file())
            stored.unlink()
            missing = self.read(media_id, 'playback')['data']
            self.assertEqual(missing['description']['state'], 'missing')
            self.assertEqual(missing['format'], 'unavailable')
            self.assertFalse(stored.exists(), 'reading historical files must not trigger backfill')


if __name__ == '__main__':
    names = schema.TEST_ARGS or ['test_description_and_original', 'test_non_video_and_missing_description',
                                 'test_cover_and_corrupt_description']
    suite = unittest.TestSuite(MediaCapabilitiesTest(name) for name in names)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not result.wasSuccessful())
