import argparse
import array
import json
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest
import wave


parser = argparse.ArgumentParser()
parser.add_argument('--tool', type=pathlib.Path, required=True)
parser.add_argument('--sample', type=pathlib.Path, required=True)
parser.add_argument('--silent', type=pathlib.Path, required=True)
options, test_arguments = parser.parse_known_args()


class AudioDecodeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='audio-decode-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = pathlib.Path(cls.temporary.name)
        cls.stereo = cls.root / 'stereo.wav'
        values = array.array('h', (value for i in range(257)
                                   for value in (i * 50 - 6000, 6000 - i * 30)))
        cls.pcm_bytes = values.tobytes()
        with wave.open(str(cls.stereo), 'wb') as output:
            output.setparams((2, 2, 48000, 0, 'NONE', 'not compressed'))
            output.writeframes(cls.pcm_bytes)
        cls.mono = cls.root / 'mono.wav'
        cls.run_ffmpeg('-f', 'lavfi', '-i', 'sine=frequency=660:sample_rate=32000:duration=0.137',
                       '-c:a', 'pcm_s16le', str(cls.mono))
        cls.multiaudio = cls.root / 'two_audio.mp4'
        cls.run_ffmpeg('-i', str(options.sample), '-i', str(cls.mono),
                       '-map', '1:a', '-map', '0:v', '-map', '0:a',
                       '-c:v', 'copy', '-c:a', 'aac', '-t', '0.1', str(cls.multiaudio))

    @classmethod
    def run_ffmpeg(cls, *arguments):
        result = subprocess.run(['ffmpeg', '-v', 'error', '-nostdin', *arguments],
                                capture_output=True, timeout=30)
        if result.returncode != 0 or result.stderr:
            raise AssertionError(result.stderr.decode('utf-8', errors='replace'))
        return result.stdout

    def run_tool(self, *arguments):
        self.assertTrue(options.tool.is_file(), 'audio_decode has not been implemented/built')
        return subprocess.run([str(options.tool), *map(str, arguments)],
                              capture_output=True, encoding='utf-8', timeout=30)

    def decode(self, source):
        output = self.root / (self._testMethodName + '.pcm')
        result = self.run_tool(source, output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, '')
        records = {}
        for line in result.stdout.splitlines():
            kind, fields = line.split(' ', 1)
            self.assertIn(kind, ('audio', 'target', 'frame', 'convert', 'summary'))
            # 声道布局描述可能是 "1 channels"，字段值不能按空格切开。
            records.setdefault(kind, []).append(dict(
                re.findall(r'(\w+)=(.*?)(?=\s+\w+=|$)', fields)))
        summary = records['summary'][0]
        pcm = output.read_bytes()
        self.assertEqual(int(summary['output_bytes']), len(pcm))
        self.assertEqual(int(summary['output_samples']) * 2 * 2, len(pcm))
        self.assertEqual(summary['decoder_eof'], 'yes')
        self.assertAlmostEqual(float(summary['duration_s']), len(pcm) / 192000, places=6)
        self.assertEqual(int(summary['frames']), len(records['frame']))
        self.assertEqual(int(summary['input_samples']),
                         sum(int(frame['nb_samples']) for frame in records['frame']))
        reference = self.run_ffmpeg('-i', str(source), '-map', '0:a:0', '-ar', '48000',
                                    '-ac', '2', '-c:a', 'pcm_s16le', '-f', 's16le', 'pipe:1')
        self.assertEqual(pcm, reference)
        return records, pcm

    def test_pcm_stereo_is_unchanged(self):
        records, pcm = self.decode(self.stereo)
        self.assertEqual(pcm, self.pcm_bytes)
        self.assertEqual(records['frame'][0]['sample_fmt'], 's16')
        self.assertEqual(records['frame'][0]['planar'], 'no')

    def test_aac_planar_frames_and_resampler_drain(self):
        records, _ = self.decode(options.sample)
        probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'a:0',
                                '-show_frames', '-show_streams', '-of', 'json', str(options.sample)],
                               capture_output=True, encoding='utf-8', timeout=30)
        self.assertEqual(probe.returncode, 0, probe.stderr)
        self.assertEqual(probe.stderr, '')
        expected = json.loads(probe.stdout)
        self.assertEqual(int(records['audio'][0]['stream']), expected['streams'][0]['index'])
        self.assertEqual(len(records['frame']), len(expected['frames']))
        for actual, frame in zip(records['frame'], expected['frames']):
            self.assertEqual(int(actual['nb_samples']), frame['nb_samples'])
            self.assertEqual(actual['pts'], str(frame.get('best_effort_timestamp', 'unknown')))
            self.assertEqual(actual['sample_fmt'], frame['sample_fmt'])
            self.assertEqual(actual['planar'], 'yes')
            self.assertEqual(int(actual['sample_rate']), 44100)
        self.assertGreater(int(records['summary'][0]['resampler_tail_samples']), 0)

    def test_mono_resampling_and_channel_conversion(self):
        records, pcm = self.decode(self.mono)
        self.assertEqual(int(records['frame'][0]['channels']), 1)
        self.assertEqual(int(records['frame'][0]['sample_rate']), 32000)
        samples = array.array('h')
        samples.frombytes(pcm)
        self.assertEqual(samples[::2], samples[1::2])

    def test_first_audio_track(self):
        records, _ = self.decode(self.multiaudio)
        self.assertEqual(int(records['audio'][0]['stream']), 0)

    def test_unicode_paths(self):
        source = self.root / '音频输入.wav'
        output = self.root / '音频输出.pcm'
        shutil.copyfile(self.stereo, source)
        result = self.run_tool(source, output)
        self.assertEqual((result.returncode, result.stderr), (0, ''))
        self.assertEqual(output.read_bytes(), self.pcm_bytes)

    def test_no_audio(self):
        output = self.root / 'no-audio.pcm'
        result = self.run_tool(options.silent, output)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, 'error: no audio stream\n')
        self.assertFalse(output.exists())

    def test_existing_output_and_input_are_preserved(self):
        for output in (self.root / 'existing.pcm', self.stereo):
            with self.subTest(output=output):
                if not output.exists():
                    output.write_bytes(b'keep this file')
                original = output.read_bytes()
                result = self.run_tool(self.stereo, output)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stderr, 'error: output file already exists\n')
                self.assertEqual(output.read_bytes(), original)

    def test_usage_and_missing_paths(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stderr, 'usage: audio_decode <input> <output.pcm>\n')
        output = self.root / 'missing.pcm'
        result = self.run_tool(self.root / 'absent.mp4', output)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, 'error: input file does not exist\n')
        self.assertFalse(output.exists())
        result = self.run_tool(self.stereo, self.root / 'absent' / 'output.pcm')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, 'error: cannot create output file\n')

    def test_invalid_media(self):
        source = self.root / 'invalid.bin'
        source.write_bytes(b'This is not an audio or video file.\n')
        output = self.root / 'invalid.pcm'
        result = self.run_tool(source, output)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr,
                         'error: avformat_open_input: Invalid data found when processing input\n')
        self.assertFalse(output.exists())


if __name__ == '__main__':
    unittest.main(argv=['audio_decode_test', *test_arguments], verbosity=2)
