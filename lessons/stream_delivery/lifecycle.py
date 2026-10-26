"""在真实隔离服务上观察连接身份、授权寿命和部分失败；运行于本机 WSL。"""
import argparse
import http.client
import http.server
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

BASE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(BASE / 'tests'))
from media_service_test import MediaServiceTest, port
import media_schema_test as schema


def wait(check, name, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(.1)
    raise RuntimeError('等待超时: ' + name)


def http_status(url, headers=None):
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers=headers or {}), timeout=8) as response:
            # 对持续 FLV 仅检查有界前缀，避免状态探测吞下整个持续响应。
            return response.status, response.read(65536)
    except urllib.error.HTTPError as error:
        return error.code, error.read()
    except urllib.error.URLError:
        return 0, b''


def redacted(text):
    return re.sub(r'(?i)((?:token|secret)=|Bearer )[0-9a-f]+', r'\1[redacted]', text)


class ControlRelay:
    """转发固定本机控制入口；延迟真实回调或断开 DELETE，不伪造成功响应。"""
    def __init__(self, upstream, emit):
        self.upstream = upstream
        self.emit = emit
        self.delay_client = ''
        self.held = threading.Event()
        self.release = threading.Event()
        self.forwarded = threading.Event()
        self.accepted = False
        self.block_delete = False
        self.blocked = 0
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def relay(self):
                body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
                delayed = False
                if self.command == 'POST' and self.path.startswith('/srs?'):
                    event = json.loads(body)
                    if event.get('action') == 'on_unpublish' and event.get('client_id') == owner.delay_client:
                        delayed = True
                        owner.held.set()
                        owner.emit('callback_held', client_id=owner.delay_client)
                        if not owner.release.wait(12):
                            self.close_connection = True
                            return
                if self.command == 'DELETE' and owner.block_delete:
                    owner.blocked += 1
                    owner.emit('management_connection_closed', count=owner.blocked)
                    self.close_connection = True
                    return
                connection = http.client.HTTPConnection('127.0.0.1', owner.upstream, timeout=10)
                try:
                    headers = {key: value for key, value in self.headers.items()
                               if key.lower() not in ('host', 'connection', 'transfer-encoding')}
                    connection.request(self.command, self.path, body, headers)
                    response = connection.getresponse()
                    payload = response.read()
                    if delayed:
                        owner.accepted = response.status == 200 and payload == b'0'
                        owner.emit('callback_forwarded', status=response.status,
                                   accepted=payload == b'0', client_id=owner.delay_client)
                        owner.forwarded.set()
                    self.send_response(response.status)
                    self.send_header('Content-Length', str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                except (OSError, http.client.HTTPException):
                    self.close_connection = True
                finally:
                    connection.close()

            do_GET = relay
            do_POST = relay
            do_DELETE = relay

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def port(self):
        return self.server.server_port

    def close(self):
        self.release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()


class Flow:
    """持续消费一个已获准的真实 HTTP-FLV 响应，统计字节而不缓存整段直播。"""
    def __init__(self, descriptor):
        self.response = urllib.request.urlopen(urllib.request.Request(
            descriptor['url'], headers=descriptor['headers']), timeout=8)
        self.bytes = 0
        self.failure = None
        self.stopping = False
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        try:
            while not self.stopping:
                block = self.response.read1(16384)
                if not block:
                    break
                self.bytes += len(block)
        except (OSError, ValueError, http.client.HTTPException) as error:
            if not self.stopping:
                self.failure = type(error).__name__

    def close(self):
        self.stopping = True
        self.thread.join(timeout=9)
        self.response.close()
        self.thread.join(timeout=1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', default='/mnt/d/chat/_server/Output/linux-x64/rtc/ChatServer')
    parser.add_argument('--srs', default='/mnt/d/chat/_server/Output/linux-x64/rtc/srs')
    parser.add_argument('--nginx', default='/opt/chat-build/deps/runtime-tools/usr/sbin/nginx')
    parser.add_argument('--skip-expiry', action='store_true', help='仅诊断其他场景；结果明确标为未验证到期')
    parser.add_argument('--sudo', action='store_true', help='复用本机 MySQL 管理员认证')
    args = parser.parse_args()
    # 夹具只创建随机数据库与仅授权该库的账号，不访问既有业务数据。
    schema.ARGS.sudo = args.sudo
    schema.MYSQL = (['sudo', '-n'] if args.sudo else []) + ['mysql', '--batch', '--raw', '--skip-column-names']
    for path in (args.server, args.srs, args.nginx):
        if not Path(path).is_file():
            parser.error('依赖不存在: ' + path)
    output = BASE / 'lessons/stream_delivery/build' / ('lifecycle-' + time.strftime('%Y%m%d-%H%M%S'))
    output.mkdir(parents=True, exist_ok=False)
    events, locks = [], threading.Lock()
    started = time.monotonic()

    def emit(event, **fields):
        record = dict(elapsed_s=round(time.monotonic() - started, 3), event=event, **fields)
        with locks:
            events.append(record)
            print(json.dumps(record, ensure_ascii=False), flush=True)

    root = Path(tempfile.mkdtemp(prefix='chat-lifecycle-'))
    processes, logs, relays, flows = [], [], [], []
    passed = False
    try:
        rtmp, api, origin, auth, public, udp = [port() for _ in range(6)]
        hook = ControlRelay(auth, emit)
        management = ControlRelay(api, emit)
        relays.extend([hook, management])
        secret = secrets.token_hex(32)
        config = (BASE / 'deploy/srs-chat.conf.in').read_text()
        replacements = {'__LIVE_ROOT__': str(root), '__HOOK_SECRET__': secret,
                        'listen 1935;': f'listen {rtmp};', '127.0.0.1:1985': f'127.0.0.1:{api}',
                        '127.0.0.1:8080': f'127.0.0.1:{origin}', '127.0.0.1:6003': f'127.0.0.1:{hook.port}',
                        'listen 8000;': f'listen {udp};', 'candidate 39.105.18.142;': 'candidate 127.0.0.1;'}
        for before, after in replacements.items():
            config = config.replace(before, after)
        (root / 'srs.conf').write_text(config)
        nginx = (BASE / 'deploy/nginx-chat-live.conf').read_text().replace(
            '127.0.0.1:6003', f'127.0.0.1:{auth}').replace('127.0.0.1:8080', f'127.0.0.1:{origin}')
        (root / 'nginx.conf').write_text(f'pid {root}/nginx.pid; error_log {root}/nginx-error.log;\n'
            f'events {{}}\nhttp {{ access_log {root}/access.log; client_body_temp_path {root}/body; '
            f'proxy_temp_path {root}/proxy; fastcgi_temp_path {root}/fastcgi; uwsgi_temp_path {root}/uwsgi; '
            f'scgi_temp_path {root}/scgi; server {{ listen 127.0.0.1:{public}; {nginx} }} }}')
        os.environ.update(CHAT_SERVER_TEST_BINARY=args.server, CHAT_LIVE_URL=f'http://127.0.0.1:{public}/live',
            CHAT_LIVE_RTMP_URL=f'rtmp://127.0.0.1:{rtmp}/live', CHAT_LIVE_RTC_URL='',
            CHAT_LIVE_HOOK_SECRET=secret, CHAT_LIVE_PORT=str(auth), CHAT_LIVE_API_PORT=str(management.port))
        MediaServiceTest.setUpClass()
        sender, receiver = MediaServiceTest.clients[:2]
        conversation, viewing = dict(is_group=False, target=2), dict(is_group=False, target=1)
        for name, command in [('srs', [args.srs, '-c', str(root / 'srs.conf')]),
                              ('nginx', [args.nginx, '-p', str(root), '-c', str(root / 'nginx.conf'), '-g', 'daemon off;'])]:
            check_command = command[:1] + ['-t'] + command[1:]
            check = subprocess.run(check_command, capture_output=True, text=True, timeout=12)
            if check.returncode:
                raise RuntimeError(redacted(check.stdout + check.stderr))
            log = open(root / (name + '.log'), 'w+')
            logs.append(log)
            processes.append(subprocess.Popen(command, cwd=root, stdout=log, stderr=log))
        wait(lambda: http_status(f'http://127.0.0.1:{api}/api/v1/versions')[0] == 200, 'SRS 启动')
        emit('environment_ready', srs=json.loads(http_status(f'http://127.0.0.1:{api}/api/v1/versions')[1])['data'],
             database=schema.MediaSchemaTest.database)
        sample = root / 'source.mp4'
        made = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
            'testsrc2=size=320x180:rate=25', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
            '-t', '6', '-c:v', 'libx264', '-preset', 'ultrafast', '-g', '25', '-bf', '0',
            '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '64k', str(sample)], capture_output=True, text=True, timeout=30)
        assert made.returncode == 0 and not made.stderr, made.stderr
        created = sender.request('live_create', conversation=conversation, client_msg_id=str(uuid.uuid4()), title='生命周期实验')
        assert created['ok'], created
        identifier = created['data']['live']['live_id']

        def state():
            result = sender.request('live_status', conversation=conversation, live_id=identifier)
            assert result['ok'], result
            return result['data']['live']

        def publish(descriptor):
            log = open(root / ('publisher-' + str(len(processes)) + '.log'), 'w+')
            logs.append(log)
            process = subprocess.Popen(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-re', '-stream_loop', '-1',
                '-i', str(sample), '-c', 'copy', '-f', 'flv', descriptor['url']], stdout=log, stderr=log)
            processes.append(process)
            return process

        def descriptor(format):
            result = receiver.request('live_read', conversation=viewing, live_id=identifier, format=format)
            assert result['ok'], result
            return result['data']['download']

        publisher = publish(created['data']['publish'])
        wait(lambda: state()['state'] == 'live', '发布回调')
        previous = state()['publisher_id']
        emit('published', live_id=identifier, publisher_id=previous)
        hook.delay_client = previous
        publisher.terminate()
        publisher.wait(timeout=10)
        wait(hook.held.is_set, '真实退出回调进入中转')
        refreshed = sender.request('live_publish', conversation=conversation, live_id=identifier)
        assert refreshed['ok'], refreshed
        publisher = publish(refreshed['data']['publish'])
        wait(lambda: state()['state'] == 'live' and state()['publisher_id'] != previous, '另一发布连接进入')
        current = state()['publisher_id']
        hook.release.set()
        wait(hook.forwarded.is_set, '迟到回调实际转发')
        assert hook.accepted, '真实退出回调未被业务服务接受'
        assert state()['state'] == 'live' and state()['publisher_id'] == current
        emit('stale_callback_safe', previous_client=previous, current_client=current, state=state()['state'])
        # 一个 HTTP 响应跨过建连票据截止时间；每次新 HLS 请求仍重新授权。
        flv, hls = descriptor('flv'), descriptor('hls')
        wait(lambda: http_status(hls['url'], hls['headers'])[0] == 200, '授权 HLS 列表')
        assert http_status(hls['url'])[0] == 403
        flow = Flow(flv)
        flows.append(flow)
        wait(lambda: flow.bytes > 65536, '已打开 FLV 响应有媒体推进')
        if not args.skip_expiry:
            # 服务端以 steady_clock 判断五分钟；WSL 的墙钟校时不能提前结束等待。
            deadline = time.monotonic() + 301
            while time.monotonic() < deadline:
                remaining = max(0, deadline - time.monotonic())
                emit('expiry_wait', remaining_s=round(remaining), flv_bytes=flow.bytes)
                time.sleep(min(30, remaining))
            assert http_status(flv['url'], flv['headers'])[0] == 403
            assert http_status(hls['url'], hls['headers'])[0] == 403
            before = flow.bytes
            wait(lambda: flow.bytes > before + 16384, '过期后已建立 FLV 响应继续流动', 8)
            renewed = descriptor('hls')
            assert http_status(renewed['url'], renewed['headers'])[0] == 200
            emit('ticket_expired', new_flv_status=403, new_hls_status=403, established_flv_delta=flow.bytes-before,
                 renewed_hls_status=200, flow_error=flow.failure)
        else:
            emit('expiry_not_run')
        # 输出为空的媒体解码验证仍走真实授权和 FFmpeg 解封装/解码。
        hls = descriptor('hls')
        headers = ''.join(key + ': ' + value + '\r\n' for key, value in hls['headers'].items())
        decoded = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-live_start_index', '-1',
            '-headers', headers, '-i', hls['url'], '-t', '2', '-map', '0:v:0', '-map', '0:a:0', '-f', 'null', '-'],
            capture_output=True, text=True, timeout=30)
        assert decoded.returncode == 0 and not decoded.stderr, redacted(decoded.stderr)
        emit('reconnected_hls_decoded', seconds=2)
        management.block_delete = True
        ended = sender.request('live_end', conversation=conversation, live_id=identifier)
        assert ended['ok'] and ended['data']['live']['state'] == 'ended', ended
        wait(lambda: management.blocked > 0, '结束触发真实 DELETE 失败')
        assert publisher.poll() is None
        before = flow.bytes
        wait(lambda: flow.bytes > before + 8192, '业务结束后媒体暂未断开', 8)
        assert http_status(hls['url'], hls['headers'])[0] == 403
        emit('end_partially_applied', business_state=state()['state'], publisher_alive=True,
             established_flv_delta=flow.bytes-before, new_hls_status=403)
        management.block_delete = False
        wait(lambda: publisher.poll() is not None, '状态核对重试断开发布者', 15)
        wait(lambda: state()['publisher_id'] == '', '退出回调清理连接身份', 10)
        assert state()['state'] == 'ended'
        emit('end_reconciled', state=state()['state'], publisher_id=state()['publisher_id'],
             blocked_management_requests=management.blocked)
        passed = True
    finally:
        cleanup_errors = []

        def cleanup(action):
            try:
                action()
            except Exception as error:
                cleanup_errors.append(redacted(str(error)))

        for relay in relays:
            relay.release.set()
            relay.block_delete = False
        for process in reversed(processes):
            if process.poll() is None:
                cleanup(process.terminate)
                cleanup(lambda process=process: process.wait(timeout=15))
        for flow in flows:
            cleanup(flow.close)
        if getattr(MediaServiceTest, 'directory', None):
            cleanup(MediaServiceTest.log.flush)
            log_path = Path(MediaServiceTest.directory.name) / 'server.log'
            cleanup(lambda: (output / 'chatserver.log').write_text(redacted(log_path.read_text())))
        cleanup(MediaServiceTest.doClassCleanups)
        for fixture in (MediaServiceTest, schema.MediaSchemaTest):
            cleanup_errors.extend(redacted(str(item[1])) for item in getattr(fixture, 'tearDown_exceptions', []))
        for relay in relays:
            cleanup(relay.close)
        for log in logs:
            cleanup(log.close)
        for path in root.glob('*.log'):
            cleanup(lambda path=path: (output / path.name).write_text(redacted(path.read_text(errors='replace'))))
        if all(process.poll() is not None for process in processes):
            cleanup(lambda: shutil.rmtree(root))
        else:
            cleanup_errors.append('实验进程仍存活，保留临时目录: ' + str(root))
        if cleanup_errors:
            passed = False
            emit('cleanup_failed', errors=cleanup_errors)
        result = dict(passed=passed, expiry_verified=passed and not args.skip_expiry,
                      elapsed_s=round(time.monotonic()-started, 3), events=events)
        (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2))
        print('RESULT ' + str(output / 'result.json'), flush=True)
    if not passed:
        raise RuntimeError('实验未通过，检查 result.json 与日志')


if __name__ == '__main__':
    main()
