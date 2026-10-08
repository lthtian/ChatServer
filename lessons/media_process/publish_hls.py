"""Publish a completed local HLS bundle to one existing chat media object.

The C++ tool does encoding/packaging. This operator tool verifies hashes, uploads
immutable objects, and atomically replaces the small ready catalog. No remote
compiler or transcoder is invoked. Passwords are prompted, never saved.
"""
import argparse
import getpass
import hashlib
import json
import pathlib
import re
import shlex
import stat
import sys
import uuid

# An optional project-local Python dependency directory keeps the system clean.
sys.path.insert(0, str(pathlib.Path(__file__).parent / 'build' / 'python'))
import paramiko

ASSET = re.compile(r'master\.m3u8|[0-9]{1,4}p/(?:index\.m3u8|init\.mp4|seg_[0-9]{5}\.(?:ts|m4s))')


def digest(path):
    with open(path, 'rb') as file:
        return hashlib.file_digest(file, 'sha256').hexdigest()


def remote(ssh, command):
    _, out, err = ssh.exec_command(command, timeout=60)
    text = out.read().decode()
    errors = err.read().decode()
    if out.channel.recv_exit_status():
        raise RuntimeError(errors or 'remote command failed')
    return text


def validate(folder):
    path = folder / 'catalog.json'
    if path.stat().st_size > 512 * 1024:
        raise ValueError('catalog too large')
    catalog = json.loads(path.read_text(encoding='utf-8'))
    if catalog['version'] != 1 or not re.fullmatch('[0-9a-f]{32}', catalog['revision']):
        raise ValueError('invalid catalog version/revision')
    assets = {a['path']: a for a in catalog['assets']}
    if len(assets) != len(catalog['assets']) or not 1 <= len(assets) <= 4096:
        raise ValueError('invalid asset count')
    if sum(a['bytes'] for a in assets.values()) > 256 * 1024 * 1024:
        raise ValueError('bundle too large')
    for name, asset in assets.items():
        if not ASSET.fullmatch(name):
            raise ValueError('invalid asset path')
        file = folder / name
        if file.is_symlink() or not file.resolve().is_relative_to(folder.resolve()):
            raise ValueError('asset escapes bundle')
        if file.stat().st_size != asset['bytes'] or digest(file) != asset['sha256']:
            raise ValueError('asset hash/length mismatch: ' + name)
        if name.endswith('.m3u8'):
            # Lists may only refer to the allowlisted relative objects of this bundle.
            for line in file.read_text(encoding='utf-8').splitlines():
                uris = re.findall(r'URI="([^"]+)"', line) if line.startswith('#') else [line]
                for uri in uris:
                    if not uri:
                        continue
                    target = str(pathlib.PurePosixPath(name).parent / uri)
                    if target not in assets or '..' in uri or ':' in uri or uri.startswith('/'):
                        raise ValueError('playlist contains an unlisted resource')
    variants = catalog['variants']
    heights = [v['height'] for v in variants]
    if not 1 <= len(variants) <= 3 or heights != sorted(set(heights), reverse=True):
        raise ValueError('invalid rendition ladder')
    for variant in variants:
        if variant['height'] > catalog['source_height'] or variant['id'] != str(variant['height']) + 'p':
            raise ValueError('invalid/upscaled rendition')
        if variant['playlist'] != variant['id'] + '/index.m3u8' or variant['playlist'] not in assets:
            raise ValueError('missing rendition playlist')
    return catalog


def publish(ssh, folder, media_id, root='/home/lth/chat-media'):
    if not re.fullmatch('[0-9a-f]{32}', media_id):
        raise ValueError('media_id must be 32 lowercase hex characters')
    catalog = validate(folder)
    base = f'{root}/objects/{media_id}'
    # The immutable source is authoritative; a bundle cannot be attached to another video.
    if remote(ssh, 'sha256sum -- ' + shlex.quote(base + '/original')).split()[0] != catalog['source_sha256']:
        raise ValueError('bundle does not match server original')
    sftp = ssh.open_sftp()

    def mkdir(path):
        try:
            sftp.mkdir(path, mode=0o700)
        except OSError:
            if not stat.S_ISDIR(sftp.lstat(path).st_mode):
                raise

    def atomic_json(name, value):
        temporary = base + '/hls/' + name + '_' + uuid.uuid4().hex
        with sftp.file(temporary, 'w') as file:
            file.write(json.dumps(value).encode())
            file.flush()
        sftp.posix_rename(temporary, base + '/hls/' + name)

    mkdir(base + '/hls')
    lock = base + '/hls/publish_lock'
    # mkdir is exclusive: a second publisher must not mix files with this task.
    sftp.mkdir(lock, mode=0o700)
    try:
        atomic_json('status', {'state': 'processing'})
        revision = base + '/hls/' + catalog['revision']
        sftp.mkdir(revision, mode=0o700)
        created = set()
        for asset in catalog['assets']:
            name = asset['path']
            target = revision + '/' + name.replace('.', '_')
            parent = target.rsplit('/', 1)[0]
            if parent != revision and parent not in created:
                mkdir(parent)
                created.add(parent)
            sftp.put(str(folder / name), target, confirm=True)
            sftp.chmod(target, 0o600)
        # Verify remote bytes once, before making the bundle visible to readers.
        checks = ''.join(a['sha256'] + '  ' + revision + '/' + a['path'].replace('.', '_') + '\n'
                         for a in catalog['assets'])
        channel_in, out, err = ssh.exec_command('sha256sum --check --status', timeout=120)
        channel_in.write(checks)
        channel_in.channel.shutdown_write()
        errors = err.read().decode()
        if out.channel.recv_exit_status():
            raise RuntimeError(errors or 'remote asset hash mismatch')
        atomic_json('catalog', catalog)
        atomic_json('status', {'state': 'ready'})
        print(json.dumps({'media_id': media_id, 'revision': catalog['revision'],
                          'renditions': [v['id'] for v in catalog['variants']]}, ensure_ascii=False))
    except Exception:
        atomic_json('status', {'state': 'failed'})
        raise
    finally:
        sftp.rmdir(lock)
        sftp.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('media_id')
    parser.add_argument('bundle', type=pathlib.Path)
    parser.add_argument('--host', default='39.105.18.142')
    parser.add_argument('--user', default='lth')
    args = parser.parse_args()
    with paramiko.SSHClient() as ssh:
        ssh.load_system_host_keys()
        ssh.connect(args.host, username=args.user, password=getpass.getpass('SSH password: '),
                    look_for_keys=False, allow_agent=False, timeout=15)
        publish(ssh, args.bundle.resolve(), args.media_id)


if __name__ == '__main__':
    main()
