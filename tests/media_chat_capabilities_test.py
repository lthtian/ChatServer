"""Run native Qt chat windows against an isolated real Linux ChatServer."""
import json
import os
from pathlib import Path
import re
import subprocess
import time

from media_service_test import MediaServiceTest
import media_schema_test as schema


def windows(path):
    return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()


def check_diagnostics(stderr, name, host=r'127\.0\.0\.1:\d+'):
    lines, canceled, active = [], [], None
    interruption = (
        r'\[mov,mp4,m4a,3gp,3g2,mj2 @ [0-9a-fA-F]+\] Could not find codec parameters for stream \d+ .*: unspecified pixel format',
        r"Consider increasing the value for the 'analyzeduration' \(\d+\) and 'probesize' \(\d+\) options",
        r'\[mov,mp4,m4a,3gp,3g2,mj2 @ [0-9a-fA-F]+\] Packet corrupt \(stream = \d+, dts = -?\d+\)\.',
        r'\[h264 @ [0-9a-fA-F]+\] Invalid NAL unit size \(\d+ > \d+\)\.',
        r'\[h264 @ [0-9a-fA-F]+\] missing picture in access unit with size \d+',
    )
    for line in stderr.splitlines():
        if not line: continue
        boundary = re.fullmatch(r'(BEGIN|END) cancel (online|cached|repaired) input', line)
        if boundary:
            if boundary[1] == 'BEGIN':
                if active is not None: raise RuntimeError('Nested input cancellation')
                active = boundary[2]
            else:
                if active != boundary[2]: raise RuntimeError('Unmatched input cancellation')
                active = None
            continue
        if active and any(re.fullmatch(pattern, line) for pattern in interruption):
            canceled.append(line)
            continue
        if not re.fullmatch(r"\[hls @ [0-9a-fA-F]+\] Opening 'http://" + host +
                r"/media/hls/[0-9a-f]{32}/(?:360|480|720)p/seg_\d+\.ts' for reading", line):
            lines.append(line)
    if active is not None: raise RuntimeError('Input cancellation did not finish')
    # 公开 H.264/HEVC 样本实际为全范围 yuvj420p；允许这一精确弃用提示，拒绝其他诊断。
    if name == 'video-vp9-opus.webm':
        if lines: raise RuntimeError('Unexpected VP9 diagnostics: ' + stderr)
    elif not lines or any(not re.fullmatch(
            r'\[swscaler @ [0-9a-fA-F]+\] deprecated pixel format used, make sure you did set range correctly',
            line) for line in lines):
        raise RuntimeError('Unexpected full-range fixture diagnostics: ' + stderr)
    return canceled


def main():
    client = Path(os.environ['CHAT_CLIENT_TEST_BINARY']).resolve()
    samples = Path(os.environ['CHAT_MEDIA_SAMPLES']).resolve()
    output = client.parent.parent / 'chat-samples'
    output.mkdir(exist_ok=True)
    reports = []
    try:
        MediaServiceTest.setUpClass()
        for name in ('video.mp4', 'video-h265.mkv', 'video-vp9-opus.webm',
                     'video-none.mp4', 'video-59.94fps.mp4', 'video-pcm.mov', 'renamed.data'):
            folder = output / name
            folder.mkdir(exist_ok=True)
            command = (f'cd /d "{windows(folder)}"&& '
                'set PATH=F:\\Qt\\Tools\\mingw810_64\\bin;F:\\Qt\\5.15.2\\mingw81_64\\bin;'
                'D:\\chat\\_deps\\ffmpeg-8.1.1-full_build-shared\\bin;C:\\Windows\\System32;C:\\Windows&& '
                'set QT_QPA_PLATFORM=offscreen&& '
                'set QT_PLUGIN_PATH=F:\\Qt\\5.15.2\\mingw81_64\\plugins&& '
                f'set CHAT_HOST=127.0.0.1&& set CHAT_PORT={MediaServiceTest.control_port}&& '
                f'set CHAT_MEDIA_URL=http://127.0.0.1:{MediaServiceTest.http_port}&& '
                f'"{windows(client)}" "{windows(samples / name)}"')
            started = time.monotonic()
            launch = folder / 'launch.cmd'
            launch.write_bytes(('@echo off\r\n' + command + '\r\n').encode('utf-8'))
            result = subprocess.run(['/mnt/c/Windows/System32/cmd.exe', '/d', '/c', windows(launch)],
                capture_output=True, timeout=180)
            log = (result.stdout + result.stderr).decode('utf-8', errors='replace')
            (folder / 'chat.log').write_text(log)
            if result.returncode or 'PASS: real video upload' not in log or 'FAIL:' in log:
                raise RuntimeError(f'{name}: Qt verification failed ({result.returncode}); see {folder}/chat.log')
            check_diagnostics(result.stderr.decode('utf-8', errors='replace'), name)
            reports.append(dict(sample=name, seconds=round(time.monotonic() - started, 2), result='passed'))
            print('PASS native chat sharing:', name, flush=True)
        (output / 'summary.json').write_text(json.dumps(reports, indent=2))
    finally:
        MediaServiceTest.doClassCleanups()
        errors = getattr(MediaServiceTest, 'tearDown_exceptions', []) + getattr(schema.MediaSchemaTest, 'tearDown_exceptions', [])
        if errors:
            raise RuntimeError('Fixture cleanup failed: ' + '; '.join(str(error[1]) for error in errors))


if __name__ == '__main__':
    main()
