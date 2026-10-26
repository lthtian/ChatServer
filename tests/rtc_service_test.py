"""真实 ChatServer / MySQL / SRS / Nginx 与两个原生 Qt 进程的 RTC 联调。"""
import json
import os
import pathlib
import secrets
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid

from media_service_test import MediaServiceTest, port
import media_schema_test as schema

BASE = pathlib.Path(__file__).resolve().parents[1]
OUT = pathlib.Path('/mnt/e/chat_server_qt/ChatClient-rtc/build/rtc-smoke')
WIN = r'E:\chat_server_qt\ChatClient-rtc\build\rtc-smoke'
SRS = '/mnt/d/chat/_server/Output/linux-x64/rtc/srs'
NGINX = '/opt/chat-build/deps/runtime-tools/usr/sbin/nginx'


def wait(check, name, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(.2)
    raise RuntimeError('timeout: ' + name)


def http(url, method='GET', body=None, headers=None):
    try:
        with urllib.request.urlopen(urllib.request.Request(url, data=body, headers=headers or {}, method=method), timeout=12) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()
    except urllib.error.URLError:
        return 0, b''


def main():
    root = pathlib.Path(tempfile.mkdtemp(prefix='chat-rtc-'))
    processes, windows, logs = [], [], []
    rtmp, api, origin, auth, public, udp = [port() for _ in range(6)]
    candidate = subprocess.check_output(['hostname', '-I'], text=True).split()[0]
    rtc_url = f'http://127.0.0.1:{public}/rtc'
    secret = secrets.token_hex(32)
    fixture_started = False
    try:
        config = (BASE / 'deploy/srs-chat.conf.in').read_text().replace('__LIVE_ROOT__', str(root)).replace('__HOOK_SECRET__', secret)
        for old, new in [('listen 1935;', f'listen {rtmp};'), ('127.0.0.1:1985', f'127.0.0.1:{api}'),
                         ('127.0.0.1:8080', f'127.0.0.1:{origin}'), ('127.0.0.1:6003', f'127.0.0.1:{auth}'),
                         ('listen 8000;', f'listen {udp};'), ('candidate 39.105.18.142;', f'candidate {candidate};')]:
            config = config.replace(old, new)
        (root / 'srs.conf').write_text(config)
        nginx = (BASE / 'deploy/nginx-chat-live.conf').read_text().replace('127.0.0.1:6003', f'127.0.0.1:{auth}').replace('127.0.0.1:8080', f'127.0.0.1:{origin}')
        # 只记录方法和状态，SDP、票据和会话资源路径均不进入实验日志。
        nginx = nginx.replace('access_log off;', f'access_log {root}/rtc-methods.log rtc_methods;')
        (root / 'nginx.conf').write_text(f'pid {root}/nginx.pid; error_log {root}/nginx-error.log;\n'
            f'events {{}}\nhttp {{ log_format rtc_methods "$request_method $status"; access_log off; client_body_temp_path {root}/body; proxy_temp_path {root}/proxy; '
            f'fastcgi_temp_path {root}/fastcgi; uwsgi_temp_path {root}/uwsgi; scgi_temp_path {root}/scgi; '
            f'server {{ listen {public}; {nginx} }} }}')
        os.environ.update(CHAT_LIVE_URL=f'http://127.0.0.1:{public}/live',
            CHAT_LIVE_RTMP_URL=f'rtmp://127.0.0.1:{rtmp}/live', CHAT_LIVE_RTC_URL=rtc_url,
            CHAT_LIVE_HOOK_SECRET=secret, CHAT_LIVE_PORT=str(auth), CHAT_LIVE_API_PORT=str(api),
            CHAT_SERVER_TEST_BINARY='/mnt/d/chat/_server/Output/linux-x64/rtc/ChatServer')
        MediaServiceTest.setUpClass()
        fixture_started = True
        for name, command in [('srs', [SRS, '-c', str(root / 'srs.conf')]),
            ('nginx', [NGINX, '-p', str(root), '-c', str(root / 'nginx.conf'), '-g', 'daemon off;'])]:
            log = open(OUT / (name + '-rtc.log'), 'w+')
            logs.append(log)
            processes.append(subprocess.Popen(command, cwd=root, stdout=log, stderr=log))
        wait(lambda: http(f'http://127.0.0.1:{api}/api/v1/versions')[0] == 200, 'SRS')
        sender, receiver, outsider, anonymous = MediaServiceTest.clients
        conversation = dict(is_group=False, target=2)
        fields = dict(conversation=conversation, client_msg_id=str(uuid.uuid4()), title='RTC real test', transport='webrtc')
        created = sender.request('live_create', **fields)
        assert created['ok'], created
        live = created['data']['live']
        identifier = live['live_id']
        repeated = sender.request('live_create', **fields)
        assert repeated['ok'] and not repeated['data']['created']
        assert live['transport'] == 'webrtc'
        assert not anonymous.request('live_read', conversation=conversation, live_id=identifier, format='webrtc')['ok']
        assert not outsider.request('live_read', conversation=dict(is_group=False, target=1), live_id=identifier, format='webrtc')['ok']
        descriptor = created['data']['publish']
        assert http(descriptor['url'], 'POST', b'v=0\r\n', {'Content-Type': 'application/sdp'})[0] == 403

        def launch(role, descriptor, duration):
            name = role + '.json'
            (OUT / (name + '.stop')).unlink(missing_ok=True)
            (OUT / name).write_text(json.dumps(dict(descriptor=descriptor, live=live, duration_ms=duration,
                image=WIN + r'\sample.png')))
            log = open(OUT / (role + '.log'), 'w+')
            logs.append(log)
            command = f'cd /d {WIN}&& set CHAT_LIVE_RTC_URL={rtc_url}&& debug\\rtc_smoke.exe {role} {name}'
            process = subprocess.Popen(['/mnt/c/Windows/System32/cmd.exe', '/d', '/c', command], stdout=log, stderr=log)
            processes.append(process)
            windows.append(process)
            return process

        publisher = launch('publish', descriptor, 65000)
        def live_state():
            if publisher.poll() is not None:
                raise RuntimeError('publisher exited; inspect publish.log')
            value = sender.request('live_status', conversation=conversation, live_id=identifier)
            return value['data']['live']['state'] == 'live'
        wait(live_state, 'RTC publish callback')
        time.sleep(3)
        viewer_conversation = dict(is_group=False, target=1)
        def source():
            data = receiver.request('live_read', conversation=viewer_conversation, live_id=identifier, format='webrtc')
            assert data['ok'], data
            return data['data']['download']
        assert not receiver.request('live_read', conversation=viewer_conversation, live_id=identifier, format='flv')['ok']
        viewer = launch('view', source(), 20000)
        assert viewer.wait(timeout=40) == 0, 'viewer failed; inspect view.log'
        wait(lambda: len(json.loads(http(f'http://127.0.0.1:{api}/api/v1/clients/')[1])['clients']) == 1,
             'viewer session DELETE removes only viewer')
        viewer = launch('window', source(), 20000)
        assert viewer.wait(timeout=40) == 0, 'window failed; inspect window.log'
        wait(lambda: len(json.loads(http(f'http://127.0.0.1:{api}/api/v1/clients/')[1])['clients']) == 1,
             'window session DELETE removes only viewer')
        methods = (root / 'rtc-methods.log').read_text().splitlines()
        assert methods.count('DELETE 204') >= 2, methods
        print('PASS two viewer DELETE 204 responses; publisher remains connected', flush=True)
        history = receiver.request('history', conversation=viewer_conversation, limit=10)
        assert any(m.get('live', {}).get('transport') == 'webrtc' for m in history['data']['messages'])
        ended = sender.request('live_end', conversation=conversation, live_id=identifier)
        assert ended['ok'], ended
        assert not receiver.request('live_read', conversation=viewer_conversation, live_id=identifier, format='webrtc')['ok']
        # 先证明服务器已撤销连接；Qt 收到业务结束消息后调用 Stop，而非等 ICE 超时。
        wait(lambda: not json.loads(http(f'http://127.0.0.1:{api}/api/v1/clients/')[1])['clients'], 'RTC cleanup')
        (OUT / 'publish.json.stop').touch()
        publisher.wait(timeout=10)
        text = (OUT / 'publish.log').read_text(errors='replace')
        assert publisher.returncode == 0 and 'FAIL' not in text and 'frames left' not in text, text
        print('PASS real RTC publish/view/window/permissions/history/end/cleanup', flush=True)
    finally:
        (OUT / 'publish.json.stop').touch()
        # Windows 子进程由自己的限时 Stop 退出，不能只结束 cmd 而留下推流线程。
        for process in windows:
            if process.poll() is None:
                process.wait(timeout=80)
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=15)
        if fixture_started:
            MediaServiceTest.log.flush()
            MediaServiceTest.log.seek(0)
            (OUT / 'chatserver-rtc.log').write_text(MediaServiceTest.log.read())
            MediaServiceTest.doClassCleanups()
        else:
            MediaServiceTest.doClassCleanups()
        for log in logs:
            log.close()
        if (root / 'rtc-methods.log').exists():
            (OUT / 'rtc-methods.log').write_text((root / 'rtc-methods.log').read_text())
        for name in ['publish.json', 'view.json', 'window.json', 'publish.json.stop', 'view.json.stop']:
            (OUT / name).unlink(missing_ok=True)
        # 临时 SRS 配置包含测试密钥，日志保留在独立输出目录。
        import shutil
        shutil.rmtree(root)


if __name__ == '__main__':
    main()
