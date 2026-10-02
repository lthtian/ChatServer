import argparse
import os
import re
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--test', type=Path, required=True)
parser.add_argument('--sample', type=Path, required=True)
parser.add_argument('--screenshot', type=Path, required=True)
parser.add_argument('--platform', choices=['offscreen', 'windows'], default='offscreen')
parser.add_argument('--case')
args = parser.parse_args()
if not args.test.is_file():
    raise SystemExit('FAIL: playback acceptance executable does not exist yet')


def ffmpeg(*arguments):
    result = subprocess.run(['ffmpeg', '-v', 'error', '-nostdin', *arguments],
                            capture_output=True, text=True, timeout=30)
    if result.returncode or result.stdout or result.stderr:
        raise RuntimeError(f'fixture generation failed: {result}')


with tempfile.TemporaryDirectory(prefix='qt-video-') as directory:
    root = Path(directory)
    reference = root / 'reference.png'
    short = root / '短视频.mp4'
    vfr = root / 'variable.mp4'
    audio = root / 'audio.wav'
    ffmpeg('-i', str(args.sample), '-map', '0:v:0', '-frames:v', '1',
           '-vf', 'scale=flags=bilinear', '-pix_fmt', 'rgb24', str(reference))
    ffmpeg('-f', 'lavfi', '-i', 'color=red:size=80x40:rate=8:duration=0.75',
           '-f', 'lavfi', '-i', 'sine=duration=0.75', '-map', '1:a', '-map', '0:v',
           '-vf', 'setpts=PTS+5/TB', '-c:v', 'libx264', '-bf', '3', '-c:a', 'aac',
           str(short))
    ffmpeg('-f', 'lavfi', '-i', 'testsrc2=size=64x48:rate=8', '-frames:v', '3',
           '-vf', r'setpts=if(eq(N\,0)\,0\,if(eq(N\,1)\,2\,7))/(8*TB)',
           '-fps_mode', 'vfr', '-c:v', 'libx264', '-bf', '0', str(vfr))
    ffmpeg('-f', 'lavfi', '-i', 'sine=duration=0.2', str(audio))
    environment = os.environ.copy()
    environment.update(PLAYER_SAMPLE=str(args.sample), PLAYER_REFERENCE=str(reference),
                       PLAYER_SHORT=str(short), PLAYER_VFR=str(vfr), PLAYER_AUDIO=str(audio),
                       PLAYER_MISSING=str(root / 'missing.mp4'),
                       PLAYER_SCREENSHOT=str(args.screenshot), QT_QPA_PLATFORM=args.platform)
    if os.name == 'nt':
        environment['QT_QPA_FONTDIR'] = str(Path(os.environ['WINDIR']) / 'Fonts')
    report = root / 'qt-test-results.txt'
    command = [str(args.test), '-v1', '-o', str(report) + ',txt']
    if args.case:
        command.append(args.case)
    result = subprocess.run(command, env=environment,
                            capture_output=True, encoding='utf-8', timeout=60)
    log = report.read_text(encoding='utf-8') if report.exists() else ''
    print(log, end='')
    print(result.stdout, end='')
    if result.stderr:
        print(result.stderr, end='')
    count = 3 if args.case else 8
    valid = (re.search(fr'Totals: {count} passed, 0 failed, 0 skipped', log) is not None
             and 'QWARN' not in log and 'QFATAL' not in log)
    raise SystemExit(result.returncode or (1 if result.stderr or not valid else 0))
