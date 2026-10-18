"""真实 MySQL、ChatServer、SRS、Nginx 和 FFmpeg 的直播链路验证。"""
import concurrent.futures
import json
import os
import pathlib
import secrets
import shutil
import subprocess
import time
import urllib.error
import urllib.request
import urllib.parse
import uuid

from media_service_test import MediaServiceTest, port
import media_schema_test as schema

BASE = pathlib.Path(__file__).resolve().parents[1]
OUTPUT = BASE / 'lessons/live_push/build/live-validation'
SRS = '/mnt/d/chat/_server/Output/linux-x64/Release/srs'
NGINX = '/opt/chat-build/deps/runtime-tools/usr/sbin/nginx'
SAMPLE = BASE / 'lessons/video_player/samples/sintel_trailer.mp4'


def wait(check, message, seconds=25):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(.2)
    raise RuntimeError(message)


def http_status(url, headers=None):
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers or {}), timeout=5) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()
    except urllib.error.URLError:
        return 0, b''


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name in ['qt-result.txt', 'reconnect-request', 'reconnect-done']:
        (OUTPUT / name).unlink(missing_ok=True)
    rtmp, api, origin, auth, public = [port() for _ in range(5)]
    secret = secrets.token_hex(32)
    root = pathlib.Path('/tmp') / ('chat-live-' + secrets.token_hex(6))
    root.mkdir(mode=0o700)
    processes, logs = [], []
    publisher = None
    try:
        config = (BASE / 'deploy/srs-chat.conf.in').read_text().replace('__LIVE_ROOT__', str(root))
        for old, new in [('listen 1935;', f'listen {rtmp};'), ('127.0.0.1:1985', f'127.0.0.1:{api}'),
                         ('127.0.0.1:8080', f'127.0.0.1:{origin}'), ('127.0.0.1:6003', f'127.0.0.1:{auth}')]:
            config = config.replace(old, new)
        (root / 'srs.conf').write_text(config.replace('__HOOK_SECRET__', secret))
        os.environ.update(CHAT_LIVE_URL=f'http://127.0.0.1:{public}/live',
            CHAT_LIVE_RTMP_URL=f'rtmp://127.0.0.1:{rtmp}/live', CHAT_LIVE_HOOK_SECRET=secret,
            CHAT_LIVE_PORT=str(auth), CHAT_LIVE_API_PORT=str(api))
        MediaServiceTest.setUpClass()
        fixture = MediaServiceTest
        schema.execute('INSERT INTO GroupUser VALUES(4,7,\'normal\');', schema.MediaSchemaTest.database)
        nginx = (BASE / 'deploy/nginx-chat-live.conf').read_text().replace('127.0.0.1:6003', f'127.0.0.1:{auth}').replace('127.0.0.1:8080', f'127.0.0.1:{origin}')
        (root / 'nginx.conf').write_text(f'pid {root}/nginx.pid; error_log {root}/nginx-error.log;\n'
            f'events {{}}\nhttp {{ access_log {root}/access.log; client_body_temp_path {root}/body; '
            f'proxy_temp_path {root}/proxy; fastcgi_temp_path {root}/fastcgi; uwsgi_temp_path {root}/uwsgi; '
            f'scgi_temp_path {root}/scgi; server {{ listen {public}; {nginx} }} }}')
        for command in [[SRS, '-t', '-c', str(root / 'srs.conf')], [NGINX, '-t', '-p', str(root), '-c', str(root / 'nginx.conf')]]:
            check = subprocess.run(command, capture_output=True, text=True, timeout=10)
            assert check.returncode == 0, check.stdout + check.stderr
        for name, command in [('srs', [SRS, '-c', str(root / 'srs.conf')]),
                              ('nginx', [NGINX, '-p', str(root), '-c', str(root / 'nginx.conf'), '-g', 'daemon off;'])]:
            log = open(OUTPUT / (name + '.log'), 'w+')
            logs.append(log)
            processes.append(subprocess.Popen(command, stdout=log, stderr=log, cwd=root))
        wait(lambda: http_status(f'http://127.0.0.1:{api}/api/v1/versions')[0] == 200, 'SRS startup')
        sender, receiver, outsider, anonymous = fixture.clients
        conversation = dict(is_group=True, target=7)
        fields = dict(conversation=conversation, client_msg_id=str(uuid.uuid4()), title='stage9 real live')
        created = sender.request('live_create', **fields)
        assert created['ok'], created
        live = created['data']['live']
        identifier = live['live_id']
        fields['live_id'] = identifier
        repeated = sender.request('live_create', **{k: v for k, v in fields.items() if k != 'live_id'})
        assert repeated['ok'] and not repeated['data']['created'], repeated
        assert outsider.request('live_read', conversation=conversation, live_id=identifier)['error'] == 'forbidden'
        assert not anonymous.request('live_status', conversation=conversation, live_id=identifier)['ok']
        history = receiver.request('history', conversation=conversation, limit=10)
        assert any(message.get('live', {}).get('live_id') == identifier for message in history['data']['messages']), history

        def state():
            result = sender.request('live_status', conversation=conversation, live_id=identifier)
            assert result['ok'], result
            return result['data']['live']

        def publish(descriptor):
            log = open(OUTPUT / 'publisher.log', 'a+')
            logs.append(log)
            process = subprocess.Popen(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-re',
                '-stream_loop', '-1', '-i', str(SAMPLE), '-c', 'copy', '-f', 'flv', descriptor['url']],
                stdout=log, stderr=log)
            processes.append(process)
            return process

        publisher = publish(created['data']['publish'])
        wait(lambda: state()['state'] == 'live', 'on_publish did not mark live')
        old_client = state()['publisher_id']
        descriptors = []
        for format in ['flv', 'hls']:
            response = receiver.request('live_read', conversation=conversation, live_id=identifier, format=format)
            assert response['ok'], response
            descriptors.append(response['data']['download'])
        assert http_status(descriptors[1]['url'])[0] == 403, 'manifest bypassed authorization'
        wait(lambda: http_status(descriptors[1]['url'], descriptors[1]['headers'])[0] == 200, 'HLS manifest')
        status, playlist = http_status(descriptors[1]['url'], descriptors[1]['headers'])
        segment = next(line for line in playlist.decode().splitlines() if line and not line.startswith('#'))
        segment_url = urllib.parse.urljoin(descriptors[1]['url'], segment)
        assert http_status(segment_url)[0] == 403, 'segment bypassed authorization'
        assert http_status(segment_url, descriptors[1]['headers'])[0] == 200
        assert http_status(descriptors[1]['url'].replace(identifier, '0' * 32), descriptors[1]['headers'])[0] == 403

        def decode(descriptor):
            header = ''.join(key + ': ' + value + '\r\n' for key, value in descriptor['headers'].items())
            result = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error',
                *(['-live_start_index', '-1'] if descriptor['url'].endswith('.m3u8') else []), '-headers', header,
                '-i', descriptor['url'], '-t', '2', '-map', '0:v:0', '-map', '0:a:0', '-f', 'null', '-'],
                capture_output=True, text=True, timeout=30)
            assert result.returncode == 0 and not result.stderr, result.stderr

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as viewers:
            list(viewers.map(decode, descriptors))
        print('PASS: create/history/idempotency, HTTP-FLV + HLS simultaneous decode, all HLS assets authorized', flush=True)
        publisher.terminate()
        publisher.wait(timeout=10)
        wait(lambda: state()['state'] == 'interrupted', 'on_unpublish did not mark interrupted')
        refreshed = sender.request('live_publish', conversation=conversation, live_id=identifier)
        publisher = publish(refreshed['data']['publish'])
        wait(lambda: state()['state'] == 'live', 'republish failed')
        new_client = state()['publisher_id']
        assert old_client != new_client
        stale = dict(action='on_unpublish', app='live', stream=identifier, client_id=old_client)
        request = urllib.request.Request(f'http://127.0.0.1:{auth}/srs?secret={secret}',
            data=json.dumps(stale).encode(), method='POST')
        with urllib.request.urlopen(request) as response:
            assert response.read() == b'0'
        assert state()['state'] == 'live' and state()['publisher_id'] == new_client
        print('PASS: disconnect/republish; stale callback cannot overwrite new publisher', flush=True)

        # Windows Qt 端使用第三个真实群成员；不替换网络或解码模块。
        fixture_file = OUTPUT / 'qt-fixture.json'
        fixture_file.write_text(json.dumps(dict(port=fixture.control_port, username='client_sender', password='test',
            conversation=conversation, live=live, control_dir=str(OUTPUT).replace('/mnt/d/', 'D:/'))))
        print('QT_READY: ' + str(fixture_file), flush=True)
        if os.environ.get('CHAT_LIVE_QT_RESULT'):
            result_file = pathlib.Path(os.environ['CHAT_LIVE_QT_RESULT'])
            restarted = False
            def qt_finished():
                nonlocal publisher, restarted
                if not restarted and (OUTPUT / 'reconnect-request').exists():
                    restarted = True
                    publisher.terminate()
                    publisher.wait(timeout=10)
                    wait(lambda: state()['state'] == 'interrupted', 'Qt disconnect not observed')
                    time.sleep(1)
                    refreshed = sender.request('live_publish', conversation=conversation, live_id=identifier)
                    publisher = publish(refreshed['data']['publish'])
                    wait(lambda: state()['state'] == 'live', 'Qt republish failed')
                    (OUTPUT / 'reconnect-done').write_text('ready')
                return result_file.exists()
            wait(qt_finished, 'Qt verification did not finish', 180)
            assert result_file.read_text().strip() == 'PASS', result_file.read_text()
        assert receiver.request('live_end', conversation=conversation, live_id=identifier)['error'] == 'forbidden'
        ended = sender.request('live_end', conversation=conversation, live_id=identifier)
        assert ended['ok'] and ended['data']['live']['state'] == 'ended', ended
        wait(lambda: publisher.poll() is not None, 'end did not disconnect publisher')
        assert receiver.request('live_read', conversation=conversation, live_id=identifier)['error'] == 'live_ended'
        assert http_status(segment_url, descriptors[1]['headers'])[0] == 403
        print('PASS: owner-only end, publisher disconnected, read credentials revoked', flush=True)
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
        if getattr(MediaServiceTest, 'directory', None):
            shutil.copyfile(pathlib.Path(MediaServiceTest.directory.name) / 'server.log', OUTPUT / 'server.log')
        MediaServiceTest.doClassCleanups()
        for log in logs:
            log.close()
        if root.exists():
            for name in ['nginx-error.log', 'access.log']:
                if (root / name).exists():
                    shutil.copyfile(root / name, OUTPUT / name)
            shutil.rmtree(root)
        (OUTPUT / 'qt-fixture.json').unlink(missing_ok=True)


if __name__ == '__main__':
    main()
