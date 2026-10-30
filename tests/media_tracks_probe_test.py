"""Validate container defaults and track labels with the real media processor."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

PROGRAM, SAMPLES = sys.argv[1:3]
sys.argv[1:] = []


class TracksProbeTest(unittest.TestCase):
    def probe(self, name):
        with tempfile.TemporaryDirectory(prefix='chat-tracks-probe-') as folder:
            output = Path(folder)/'description.json'
            result = subprocess.run([PROGRAM, 'probe', str(Path(SAMPLES)/name), str(output)],
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, '')
            return json.loads(output.read_text())

    def test_defaults_and_labels(self):
        for name in ('dual.mp4', 'dual.mkv'):
            with self.subTest(name=name):
                data = self.probe(name)
                self.assertEqual(data['state'], 'recognized')
                self.assertEqual(data['audio_stream'], 2)
                self.assertTrue(data['tracks'][2]['default'])
                self.assertFalse(data['tracks'][1]['default'])
                self.assertEqual(data['tracks'][2]['language'], 'zho')
                self.assertEqual(data['tracks'][2]['title'], 'Chinese 880 Hz')
        self.assertEqual(self.probe('two-video.mp4')['video_stream'], 1)


unittest.main()
