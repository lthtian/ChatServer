"""Exercise the real media processor against downloaded and malformed files."""
import json
import http.server
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

PROGRAM, SAMPLES = sys.argv[1:3]
sys.argv[1:] = []


class ProbeTest(unittest.TestCase):
    def probe(self, name):
        with tempfile.TemporaryDirectory(prefix='chat-media-probe-') as folder:
            output = Path(folder) / 'description.json'
            result = subprocess.run([PROGRAM, 'probe', str(Path(SAMPLES) / name), str(output)],
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, '')
            self.assertTrue(output.is_file(), result.stdout)
            return json.loads(output.read_text(encoding='utf-8'))

    def test_containers_and_tracks(self):
        cases = [('video.mp4', 'mov', 'h264', 'aac'),
                 ('video-h265.mkv', 'matroska', 'hevc', 'aac'),
                 ('video-vp9-opus.webm', 'matroska', 'vp9', 'opus'),
                 ('video-pcm.mov', 'mov', 'h264', 'pcm_s16le')]
        for name, demuxer, video, audio in cases:
            with self.subTest(name=name):
                data = self.probe(name)
                self.assertEqual(data['schema'], 1)
                self.assertEqual(data['state'], 'recognized')
                self.assertEqual(data['demuxer'], demuxer)
                self.assertGreater(data['duration_us'], 0)
                tracks = data['tracks']
                selected = {track['index']: track for track in tracks}
                v = selected[data['video_stream']]
                a = selected[data['audio_stream']]
                self.assertEqual(v['codec'], video)
                self.assertEqual(a['codec'], audio)
                self.assertGreater(v['width'], 0)
                self.assertGreater(v['height'], 0)
                self.assertGreater(v['bit_depth'], 0)
                self.assertGreater(a['sample_rate'], 0)
                self.assertGreater(a['channels'], 0)
                self.assertIn('sar', v)
                self.assertIn('display_matrix', v)
                self.assertIn('profile', v)
                self.assertIsInstance(v['time_base'], list)
                self.assertIn('duration_us', v)
                self.assertIn('channel_layout', a)

    def test_no_audio_and_fractional_frame_rate(self):
        self.assertIsNone(self.probe('video-none.mp4')['audio_stream'])
        data = self.probe('video-59.94fps.mp4')
        video = next(t for t in data['tracks'] if t['index'] == data['video_stream'])
        num, den = video['average_frame_rate']
        self.assertAlmostEqual(num / den, 60000 / 1001, places=2)

    def test_name_does_not_decide_content(self):
        self.assertEqual(self.probe('video.mp4'), self.probe('renamed.data'))

    def test_cover_is_not_a_video_track(self):
        data = self.probe('audio-cover.m4a')
        self.assertEqual(data['state'], 'not_video')
        self.assertEqual(data['failure'], 'no_video_track')
        self.assertIsNone(data['video_stream'])
        self.assertTrue(any(t['attached_picture'] for t in data['tracks']))

    def test_playlist_does_not_open_external_resource(self):
        requests = []
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                self.send_response(404)
                self.end_headers()
            def log_message(self, *args):
                pass
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix='chat-playlist-') as folder:
                playlist = Path(folder) / 'external.m3u8'
                playlist.write_text('#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\n'
                                    f'http://127.0.0.1:{server.server_port}/private.ts\n#EXT-X-ENDLIST\n')
                self.assertEqual(self.probe(str(playlist))['state'], 'unrecognized')
                self.assertEqual(requests, [])
        finally:
            server.shutdown()
            thread.join()
            server.server_close()

    def test_invalid_and_playlist_inputs(self):
        for name in ('invalid.mp4', 'truncated.mp4', 'external.m3u8'):
            with self.subTest(name=name):
                data = self.probe(name)
                self.assertEqual(data['state'], 'unrecognized')
                self.assertIsNone(data['video_stream'])
                self.assertTrue(data['failure'])
                self.assertRegex(data['failure'], r'^(probe_container|probe_tracks):')


unittest.main()
