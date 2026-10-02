import argparse
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest


parser = argparse.ArgumentParser()
parser.add_argument('--tool', type=pathlib.Path, required=True)
parser.add_argument('--sample', type=pathlib.Path, required=True)
parser.add_argument('--ffmpeg', required=True)
parser.add_argument('--ffprobe', required=True)
options = parser.parse_args()


class MediaInfoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not options.tool.is_file():
            raise AssertionError(f'media_info executable is missing: {options.tool}')
        if not options.sample.is_file():
            raise AssertionError(f'input sample is missing: {options.sample}')
        cls.temporary = tempfile.TemporaryDirectory(prefix='media-info-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = pathlib.Path(cls.temporary.name)
        cls.audiovisual = cls.root / 'audio_video.mp4'
        cls.audio = cls.root / 'audio.wav'
        cls.generate('-f', 'lavfi', '-i', 'testsrc2=size=64x48:rate=8:duration=1',
                     '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=1',
                     '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-c:a', 'aac',
                     '-shortest', str(cls.audiovisual))
        cls.generate('-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000:duration=1',
                     '-c:a', 'pcm_s16le', str(cls.audio))

    @classmethod
    def generate(cls, *arguments):
        result = subprocess.run([options.ffmpeg, '-v', 'error', '-nostdin', *arguments],
                                capture_output=True, encoding='utf-8', timeout=30)
        if result.returncode != 0 or result.stdout or result.stderr:
            raise AssertionError(f'fixture generation failed: {result}')

    def run_tool(self, *arguments):
        return subprocess.run([str(options.tool), *map(str, arguments)],
                              capture_output=True, encoding='utf-8', timeout=15)

    def compare_with_probe(self, source, limit=8, default_limit=False):
        arguments = [source] if default_limit else [source, limit]
        result = self.run_tool(*arguments)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, '')
        records = {}
        for line in result.stdout.splitlines():
            kind, *fields = line.split()
            self.assertIn(kind, ('format', 'stream', 'audio', 'packet', 'summary'))
            records.setdefault(kind, []).append(dict(field.split('=', 1) for field in fields))

        probe = subprocess.run(
            [options.ffprobe, '-v', 'error', '-show_format', '-show_streams',
             '-show_packets', '-read_intervals', f'%+#{limit}', '-of', 'json', str(source)],
            capture_output=True, encoding='utf-8', timeout=15)
        self.assertEqual(probe.returncode, 0, probe.stderr)
        self.assertEqual(probe.stderr, '')
        expected = json.loads(probe.stdout)
        info = records['format'][0]
        self.assertEqual(info['name'], expected['format']['format_name'])
        self.assertEqual(int(info['streams']), len(expected['streams']))
        self.assertAlmostEqual(float(info['duration_s']), float(expected['format']['duration']), places=6)
        self.assertEqual(int(info['bit_rate']), int(expected['format']['bit_rate']))

        self.assertEqual(len(records['stream']), len(expected['streams']))
        for actual, stream in zip(records['stream'], expected['streams']):
            self.assertEqual(int(actual['index']), stream['index'])
            self.assertEqual(actual['type'], stream['codec_type'])
            self.assertEqual(actual['codec'], stream['codec_name'])
            self.assertEqual(actual['time_base'], stream['time_base'])
            if stream['codec_type'] == 'video':
                self.assertEqual(int(actual['width']), stream['width'])
                self.assertEqual(int(actual['height']), stream['height'])
                self.assertEqual(actual['avg_frame_rate'], stream['avg_frame_rate'])
            elif stream['codec_type'] == 'audio':
                self.assertEqual(int(actual['sample_rate']), int(stream['sample_rate']))
                self.assertEqual(int(actual['channels']), stream['channels'])
        has_audio = any(stream['codec_type'] == 'audio' for stream in expected['streams'])
        self.assertEqual(records['audio'], [{'present': 'yes' if has_audio else 'no'}])

        self.assertEqual(len(records.get('packet', [])), len(expected['packets']))
        for index, (actual, packet) in enumerate(zip(records['packet'], expected['packets'])):
            self.assertEqual(int(actual['index']), index)
            self.assertEqual(int(actual['stream']), packet['stream_index'])
            self.assertEqual(int(actual['size']), int(packet['size']))
            self.assertEqual(actual['key'], 'yes' if 'K' in packet['flags'] else 'no')
            for timestamp in ('pts', 'dts'):
                self.assertEqual(actual[timestamp], str(packet.get(timestamp, 'unknown')))
                if timestamp in packet:
                    self.assertAlmostEqual(float(actual[f'{timestamp}_s']),
                                           float(packet[f'{timestamp}_time']), places=6)
                else:
                    self.assertEqual(actual[f'{timestamp}_s'], 'unknown')
            self.assertEqual(int(actual['duration']), packet.get('duration', 0))
            self.assertAlmostEqual(float(actual['duration_s']),
                                   float(packet.get('duration_time', 0)), places=6)
        self.assertEqual(int(records['summary'][0]['packets']), len(expected['packets']))
        return records

    def test_sample_matches_probe_in_read_order(self):
        records = self.compare_with_probe(options.sample, default_limit=True)
        self.assertEqual(records['summary'], [{'packets': '8', 'eof': 'no'}])
        self.assertEqual([packet['pts'] for packet in records['packet'][:5]],
                         ['0', '8192', '4096', '2048', '6144'])
        self.assertEqual(records['packet'][0]['dts'], '-4096')

    def test_packet_limit(self):
        records = self.compare_with_probe(options.sample, 3)
        self.assertEqual(records['summary'], [{'packets': '3', 'eof': 'no'}])

    def test_eof_is_a_successful_end(self):
        records = self.compare_with_probe(options.sample, 100)
        self.assertEqual(records['summary'], [{'packets': '40', 'eof': 'yes'}])

    def test_video_and_audio_keep_their_own_time_bases(self):
        records = self.compare_with_probe(self.audiovisual, 100)
        self.assertEqual({packet['stream'] for packet in records['packet']}, {'0', '1'})

    def test_audio_only_file(self):
        self.compare_with_probe(self.audio, 100)

    def test_unicode_path(self):
        source = self.root / '视频 样本.mp4'
        shutil.copyfile(options.sample, source)
        self.compare_with_probe(source)
        self.assertEqual(source.read_bytes(), options.sample.read_bytes())

    def test_missing_file_reports_open_failure(self):
        result = self.run_tool(self.root / 'missing.mp4')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, '')
        self.assertEqual(result.stderr,
                         'error: avformat_open_input: No such file or directory\n')

    def test_invalid_media_reports_open_failure(self):
        source = self.root / 'invalid.bin'
        source.write_bytes(b'This is not a media file.\n')
        result = self.run_tool(source)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, '')
        self.assertEqual(result.stderr,
                         'error: avformat_open_input: Invalid data found when processing input\n')

    def test_invalid_media_reports_probe_failure(self):
        source = self.root / 'invalid.dat'
        source.write_bytes(b'This is not a media file.\n')
        result = self.run_tool(source)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, '')
        self.assertRegex(
            result.stderr,
            r'\A\[luodat @ [0-9a-fA-F]+\] Format luodat detected only with low score of 1, '
            r'misdetection possible!\nerror: avformat_find_stream_info: End of file\n\Z')

    def test_invalid_arguments(self):
        for arguments in ([], [options.sample, '0'], [options.sample, '-1'],
                          [options.sample, '8x'], [options.sample, '2147483648'],
                          [options.sample, '1', 'extra']):
            with self.subTest(arguments=arguments):
                result = self.run_tool(*arguments)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, '')
                self.assertEqual(result.stderr,
                                 'usage: media_info <file> [positive-packet-count]\n'
                                 '       media_info <file> --decode <output.ppm>\n')


if __name__ == '__main__':
    unittest.main(argv=['media_info_test'], verbosity=2)
