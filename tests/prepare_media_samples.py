"""Download bounded public fixtures and record the bytes actually tested."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request

FILES = ('video.mp4', 'video-h265.mkv', 'video-vp9-opus.webm',
         'video-none.mp4', 'video-59.94fps.mp4', 'video-pcm.mov')
CHECKSUMS = {
    'video.mp4': 'f54c8c97352c81a637bd90521799ceef4927c00c54ecd77687d120ee7a32db3e',
    'video-h265.mkv': 'abb9007040779181e058e3e60208da073e245d40892f50e08d2c1d9a56adcc13',
    'video-vp9-opus.webm': 'd5ada6532171ea8838165411084377a024bcd4919b0398b2e3a476ffaed06b35',
    'video-none.mp4': '4cf512fd2ff844484f9fab029c25c15b747616c160ecef01862132b1f2f974cb',
    'video-59.94fps.mp4': 'dcf864787380a75fb0436633dcfafd0dfdb954e4dde2fb4180d5f8e0cd245354',
    'video-pcm.mov': '158b00e1b079a868dbdba6ab8eccf9a5291efc2b51941e2fff336af6e353955d',
}
CATALOG = 'https://remotion.media/'
LICENSE = 'Catalog permits royalty-free use without attribution (checked 2026-10-27).'


def prepare(destination, ffprobe, ffmpeg):
    destination.mkdir(parents=True, exist_ok=True)
    records = []
    for name in FILES:
        path = destination / name
        url = CATALOG + name
        if not path.exists():
            request = urllib.request.Request(url, headers={'User-Agent': 'ChatServer-media-tests/1.0'})
            with urllib.request.urlopen(request, timeout=30) as response:
                content = response.read(12 * 1024 * 1024 + 1)
            if len(content) > 12 * 1024 * 1024:
                raise ValueError(f'Fixture exceeds download budget: {name}')
            path.write_bytes(content)
        if path.stat().st_size > 12 * 1024 * 1024 or hashlib.sha256(path.read_bytes()).hexdigest() != CHECKSUMS[name]:
            raise ValueError(f'Fixture content differs from the recorded source: {path}')
        result = subprocess.run([ffprobe, '-v', 'error', '-show_format', '-show_streams',
                                 '-of', 'json', str(path)], capture_output=True, timeout=30, check=True)
        if result.stderr:
            raise ValueError(f'Unexpected probe diagnostic: {result.stderr.decode(errors="replace")}')
        records.append({'name': name, 'source': url, 'bytes': path.stat().st_size,
                        'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                        'probe': json.loads(result.stdout)})
        print(f'{name}: {path.stat().st_size} bytes', flush=True)
    cover = destination / 'audio-cover.m4a'
    command = [ffmpeg, '-v', 'error', '-nostdin', '-y', '-i', str(destination / 'video.mp4'),
               '-map', '0:a:0', '-map', '0:v:0', '-c:a', 'copy', '-c:v', 'mjpeg', '-frames:v', '1',
               '-disposition:v:0', 'attached_pic', str(cover)]
    result = subprocess.run(command, capture_output=True, timeout=30, check=True)
    if result.stderr:
        raise ValueError(f'Unexpected cover fixture diagnostic: {result.stderr.decode(errors="replace")}')
    (destination / 'renamed.data').write_bytes((destination / 'video.mp4').read_bytes())
    (destination / 'invalid.mp4').write_bytes(b'This is a plain text file, not video.\n')
    (destination / 'truncated.mp4').write_bytes((destination / 'video.mp4').read_bytes()[:32])
    (destination / 'external.m3u8').write_text(
        '#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\nhttp://127.0.0.1:9/private.ts\n#EXT-X-ENDLIST\n',
        encoding='utf-8')
    manifest = {'catalog': CATALOG, 'license': LICENSE, 'files': records,
                'generated': [{'name': cover.name, 'command': command, 'bytes': cover.stat().st_size,
                               'sha256': hashlib.sha256(cover.read_bytes()).hexdigest()}],
                'derived': {'renamed.data': 'byte-identical video.mp4 with a different suffix',
                            'invalid.mp4': 'plain text', 'truncated.mp4': 'first 32 bytes of video.mp4',
                            'external.m3u8': 'playlist that must not be followed by file probing'}}
    (destination / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--ffprobe', default='ffprobe')
    parser.add_argument('--ffmpeg', default='ffmpeg')
    args = parser.parse_args()
    prepare(args.directory, args.ffprobe, args.ffmpeg)
