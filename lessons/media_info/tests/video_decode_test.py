import argparse
import json
import pathlib
import subprocess
import tempfile
import unittest


parser = argparse.ArgumentParser()
parser.add_argument('--tool', type=pathlib.Path, required=True)
parser.add_argument('--sample', type=pathlib.Path, required=True)
parser.add_argument('--ffmpeg', required=True)
parser.add_argument('--ffprobe', required=True)
options = parser.parse_args()
USAGE = ('usage: media_info <file> [positive-packet-count]\n'
         '       media_info <file> --decode <output.ppm>\n')


class VideoDecodeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='video-decode-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = pathlib.Path(cls.temporary.name)
        cls.audiovisual = cls.root / 'audio_first.mp4'
        cls.audio = cls.root / 'audio.wav'
        cls.single = cls.root / 'single.mp4'
        cls.full_range = cls.root / 'full_range.mp4'
        cls.multivideo = cls.root / 'two_videos.mp4'
        cls.generate('-f', 'lavfi', '-i', 'testsrc2=size=66x50:rate=8:duration=1',
                     '-f', 'lavfi', '-i', 'sine=sample_rate=48000:duration=1',
                     '-map', '1:a', '-map', '0:v', '-c:v', 'libx264', '-bf', '3',
                     '-x264-params', 'b-adapt=0', '-pix_fmt', 'yuv420p', '-c:a', 'aac',
                     str(cls.audiovisual))
        cls.generate('-f', 'lavfi', '-i', 'sine=duration=0.2',
                     '-c:a', 'pcm_s16le', str(cls.audio))
        cls.generate('-f', 'lavfi', '-i', 'testsrc2=size=66x50:rate=8',
                     '-frames:v', '1', '-c:v', 'libx264', '-bf', '3', str(cls.single))
        cls.generate('-f', 'lavfi', '-i', 'testsrc=size=66x50:rate=8:duration=1',
                     '-vf', 'scale=out_color_matrix=bt709:out_range=full,format=yuv444p',
                     '-c:v', 'libx264', '-bf', '0', '-colorspace', 'bt709',
                     '-color_range', 'pc', '-color_primaries', 'bt709',
                     '-color_trc', 'bt709', str(cls.full_range))
        cls.generate('-f', 'lavfi', '-i', 'testsrc2=size=66x50:rate=8:duration=1',
                     '-f', 'lavfi', '-i', 'testsrc2=size=34x26:rate=8:duration=1',
                     '-map', '0:v', '-map', '1:v', '-c:v', 'libx264', str(cls.multivideo))

    @classmethod
    def generate(cls, *arguments):
        result = subprocess.run([options.ffmpeg, '-v', 'error', '-nostdin', *arguments],
                                capture_output=True, text=True, timeout=30)
        if result.returncode != 0 or result.stdout or result.stderr:
            raise AssertionError(f'fixture generation failed: {result}')

    def run_tool(self, *arguments):
        return subprocess.run([str(options.tool), *map(str, arguments)],
                              capture_output=True, encoding='utf-8', timeout=30)

    def compare_decoding(self, source, output_name='first.ppm', expected_diagnostic=None):
        output = self.root / self._testMethodName / output_name
        output.parent.mkdir()
        result = self.run_tool(source, '--decode', output)
        self.assertEqual(result.returncode, 0, result.stderr)
        if expected_diagnostic is None:
            self.assertEqual(result.stderr, '')
        else:
            self.assertRegex(result.stderr, expected_diagnostic)
        records = {}
        for line in result.stdout.splitlines():
            kind, *fields = line.split()
            self.assertIn(kind, ('format', 'stream', 'audio', 'packet', 'summary',
                                 'decoder', 'frame', 'image', 'drain', 'decode_summary'))
            records.setdefault(kind, []).append(dict(field.split('=', 1) for field in fields))

        probe = subprocess.run(
            [options.ffprobe, '-v', 'error', '-select_streams', 'v:0',
             '-show_streams', '-show_frames', '-of', 'json', str(source)],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(probe.returncode, 0, probe.stderr)
        self.assertEqual(probe.stderr, '')
        expected = json.loads(probe.stdout)
        stream = expected['streams'][0]
        frames = records['frame']
        self.assertEqual(len(frames), len(expected['frames']))
        self.assertEqual(int(records['decoder'][0]['stream']), stream['index'])
        for index, (actual, frame) in enumerate(zip(frames, expected['frames'])):
            self.assertEqual(int(actual['index']), index)
            self.assertEqual(int(actual['stream']), stream['index'])
            for field in ('width', 'height'):
                self.assertEqual(int(actual[field]), frame[field])
            self.assertEqual(actual['pix_fmt'], frame['pix_fmt'])
            self.assertEqual(actual['picture'], frame['pict_type'])
            for field in ('pts', 'best_effort_timestamp'):
                self.assertEqual(actual[field], str(frame.get(field, 'unknown')))
            self.assertAlmostEqual(float(actual['time_s']),
                                   float(frame['best_effort_timestamp_time']), places=6)
            self.assertGreaterEqual(abs(int(actual['linesize0'])), int(actual['width']))
            self.assertIn(actual['phase'], ('packets', 'drain'))
        summary = records['decode_summary'][0]
        self.assertEqual(int(summary['frames']), len(frames))
        self.assertEqual(int(summary['drained_frames']),
                         sum(frame['phase'] == 'drain' for frame in frames))
        self.assertEqual(records['drain'], [{'status': 'begin'}, {'status': 'end'}])
        self.assertEqual(records['summary'][0]['eof'], 'yes')
        self.assertEqual(len(records['image']), 1)

        magic, dimensions, maximum, pixels = output.read_bytes().split(b'\n', 3)
        self.assertEqual(magic, b'P6')
        width, height = map(int, dimensions.split())
        self.assertEqual((width, height), (stream['width'], stream['height']))
        self.assertEqual(maximum, b'255')
        self.assertEqual(len(pixels), width * height * 3)
        reference = subprocess.run(
            [options.ffmpeg, '-v', 'error', '-nostdin', '-i', str(source), '-map', '0:v:0',
             '-frames:v', '1', '-vf', 'scale=flags=bilinear', '-pix_fmt', 'rgb24',
             '-f', 'rawvideo', 'pipe:1'], capture_output=True, timeout=30)
        self.assertEqual(reference.returncode, 0, reference.stderr.decode('utf-8'))
        self.assertEqual(reference.stderr, b'')
        self.assertEqual(pixels, reference.stdout)
        return records, result.stdout

    def test_sample_outputs_all_frames_and_exact_rgb(self):
        records, trace = self.compare_decoding(options.sample)
        self.assertEqual(len(records['frame']), 40)
        self.assertGreater(int(records['decode_summary'][0]['drained_frames']), 0)
        self.assertEqual(records['frame'][-1]['time_s'], '4.875000')
        before_first_frame = trace.split('frame index=0', 1)[0]
        self.assertGreater(before_first_frame.count('packet index='), 1)

    def test_audio_first_stream_and_yuv420p(self):
        records, _ = self.compare_decoding(self.audiovisual)
        self.assertEqual(records['decoder'][0]['stream'], '1')
        self.assertEqual(records['frame'][0]['pix_fmt'], 'yuv420p')
        self.assertEqual(len(records['frame']), 8)

    def test_first_frame_can_arrive_during_draining(self):
        records, _ = self.compare_decoding(self.single)
        self.assertEqual(len(records['frame']), 1)
        self.assertEqual(records['frame'][0]['phase'], 'drain')

    def test_bt709_full_range_conversion(self):
        # H.264 full-range output uses YUVJ444P; swscale logs this specific warning.
        records, _ = self.compare_decoding(
            self.full_range,
            expected_diagnostic=(r'\A\[swscaler @ [0-9a-fA-F]+\] deprecated pixel format '
                                 r'used, make sure you did set range correctly\n\Z'))
        self.assertEqual(records['frame'][0]['pix_fmt'], 'yuvj444p')

    def test_only_selected_video_stream_is_decoded(self):
        records, _ = self.compare_decoding(self.multivideo)
        self.assertEqual(len(records['frame']), 8)
        self.assertEqual({packet['stream'] for packet in records['packet']}, {'0', '1'})

    def test_unicode_output_path(self):
        self.compare_decoding(options.sample, '第一帧 图片.ppm')

    def test_existing_output_is_not_overwritten(self):
        output = self.root / 'existing.ppm'
        output.write_bytes(b'keep this content')
        result = self.run_tool(options.sample, '--decode', output)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, 'error: output file already exists\n')
        self.assertEqual(output.read_bytes(), b'keep this content')

    def test_input_cannot_be_used_as_output(self):
        before = options.sample.read_bytes()
        result = self.run_tool(options.sample, '--decode', options.sample)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, 'error: output file already exists\n')
        self.assertEqual(options.sample.read_bytes(), before)

    def test_missing_output_directory_reports_error(self):
        output = self.root / 'missing' / 'first.ppm'
        result = self.run_tool(options.sample, '--decode', output)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, 'error: cannot create image file\n')
        self.assertFalse(output.exists())

    def test_audio_only_input_reports_missing_video(self):
        output = self.root / 'audio.ppm'
        result = self.run_tool(self.audio, '--decode', output)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, 'error: no video stream found\n')
        self.assertFalse(output.exists())

    def test_invalid_decode_arguments(self):
        for arguments in ([options.sample, '--decode'],
                          [options.sample, '--decode', ''],
                          [options.sample, '--decode', 'frame.ppm', 'extra']):
            with self.subTest(arguments=arguments):
                result = self.run_tool(*arguments)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, '')
                self.assertEqual(result.stderr, USAGE)


if __name__ == '__main__':
    unittest.main(argv=['video_decode_test'], verbosity=2)
