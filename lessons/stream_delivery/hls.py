"""Observe real Nginx HLS publication, retention, caching and request authorization."""
import argparse
import contextlib
import hashlib
import hmac
import http.server
import json
import os
from pathlib import Path
import secrets
import shutil
import threading
import time
from urllib.parse import urlsplit

from common import Process, append_json, check_ports, output_directory, request, wait_http, write_json


def playlist(path, sequence, segment):
    temporary = path.with_suffix(".pending")
    temporary.write_text(f"#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                         f"#EXT-X-MEDIA-SEQUENCE:{sequence}\n#EXTINF:1.000,\n{segment}\n", encoding="utf-8")
    os.replace(temporary, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nginx", default="/opt/chat-build/deps/runtime-tools/usr/sbin/nginx")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--output")
    parser.add_argument("--base-port", type=int, default=21982)
    args = parser.parse_args()
    origin_port, edge_port = args.base_port, args.base_port + 1
    check_ports((origin_port, edge_port))
    output = output_directory(args.output, "hls")
    print(f"output: {output}", flush=True)
    media = output / "origin"
    media.mkdir()
    for name in ("generated", "healthy", "missing", "stale", "retire", "guarded"):
        (media / name).mkdir()
    (output / "responses").mkdir()
    secret = secrets.token_bytes(32)
    events = []
    lock = threading.Lock()

    class Origin(http.server.SimpleHTTPRequestHandler):
        extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                          ".ts": "video/mp2t", ".m3u8": "application/vnd.apple.mpegurl"}

        def __init__(self, *values, **kwargs):
            super().__init__(*values, directory=str(media), **kwargs)

        def do_GET(self):
            if self.path == "/health":
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"ok")
            elif self.path == "/authorize":
                token = self.headers.get("X-Lab-Token", "")
                expires, separator, supplied = token.partition(":")
                expected = hmac.new(secret, expires.encode(), hashlib.sha256).hexdigest()
                allowed = (bool(separator) and expires.isdigit() and int(expires) > int(time.time())
                           and hmac.compare_digest(expected, supplied))
                self.send_response(204 if allowed else 401)
                self.end_headers()
            else:
                super().do_GET()

        def send_response(self, code, message=None):
            with lock:
                event = dict(index=len(events), unix_ms=int(time.time() * 1000),
                             path=urlsplit(self.path).path, status=code)
                events.append(event)
                append_json(output / "origin.jsonl", event)
            super().send_response(code, message)

        def log_message(self, *unused):
            pass

    def token(expires):
        value = str(expires)
        return value + ":" + hmac.new(secret, value.encode(), hashlib.sha256).hexdigest()

    def count(path):
        with lock:
            return sum(event["path"] == path for event in events)

    observations = []

    def observe(label, path, headers=None, origin=False):
        port = origin_port if origin else edge_port
        result, body = request(f"http://127.0.0.1:{port}{path}", headers)
        result.update(label=label, path=path, unix_ms=int(time.time() * 1000),
                      server="origin" if origin else "edge", body_bytes=len(body),
                      body_sha256=hashlib.sha256(body).hexdigest(),
                      origin_resource_requests=count(path), authorization_requests=count("/authorize"))
        (output / "responses" / f"{label}.body").write_bytes(body)
        write_json(output / "responses" / f"{label}.json", result)
        observations.append(result)
        print(f"{label}: HTTP {result['status']} cache={result['headers'].get('X-Lab-Cache', '-')}", flush=True)
        return result, body

    config = output / "nginx.conf"
    config.write_text(f"""daemon off;
master_process off;
pid {output}/nginx.pid;
error_log {output}/nginx-error.log notice;
events {{ worker_connections 64; }}
http {{
    access_log {output}/nginx-access.log combined;
    client_body_temp_path {output}/client-temp;
    proxy_temp_path {output}/proxy-temp;
    fastcgi_temp_path {output}/fastcgi-temp;
    uwsgi_temp_path {output}/uwsgi-temp;
    scgi_temp_path {output}/scgi-temp;
    proxy_cache_path {output}/cache keys_zone=lesson:1m inactive=1m max_size=8m;
    server {{
        listen 127.0.0.1:{edge_port};
        location / {{ proxy_pass http://127.0.0.1:{origin_port}; }}
        location /stale/ {{
            proxy_pass http://127.0.0.1:{origin_port};
            proxy_cache lesson;
            proxy_cache_key $scheme$proxy_host$uri;
            proxy_cache_valid 200 5s;
            add_header X-Lab-Cache $upstream_cache_status always;
        }}
        location /guarded/ {{
            auth_request /_authorize;
            proxy_pass http://127.0.0.1:{origin_port};
            proxy_cache lesson;
            proxy_cache_key $scheme$proxy_host$uri;
            proxy_cache_valid 200 1m;
            add_header X-Lab-Cache $upstream_cache_status always;
        }}
        location = /_authorize {{
            internal;
            proxy_pass http://127.0.0.1:{origin_port}/authorize;
            proxy_pass_request_body off;
            proxy_set_header Content-Length "";
            proxy_set_header X-Lab-Token $http_x_lab_token;
        }}
    }}
}}
""", encoding="utf-8")
    server = None
    try:
        with contextlib.ExitStack() as stack:
            generate = stack.enter_context(Process([args.ffmpeg, "-hide_banner", "-loglevel", "warning",
                "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=25", "-f", "lavfi", "-i",
                "sine=frequency=440:sample_rate=48000", "-t", "4", "-c:v", "libx264", "-threads", "2",
                "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-g", "25", "-sc_threshold", "0", "-bf", "0",
                "-c:a", "aac", "-b:a", "96k", "-f", "hls", "-hls_time", "1", "-hls_list_size", "0",
                "-hls_segment_filename", media / "generated" / "segment-%d.ts", media / "generated" / "index.m3u8"], output, "generate"))
            generate.wait(30)
            segment0 = media / "generated" / "segment-0.ts"
            segment1 = media / "generated" / "segment-1.ts"
            for name in ("healthy", "stale", "retire", "guarded"):
                shutil.copyfile(segment0, media / name / "segment-0.ts")
                shutil.copyfile(segment1, media / name / "segment-1.ts")
                playlist(media / name / "index.m3u8", 0, "segment-0.ts")
            server = http.server.ThreadingHTTPServer(("127.0.0.1", origin_port), Origin)
            threading.Thread(target=server.serve_forever, daemon=True).start()
            with Process([args.nginx, "-t", "-p", str(output) + "/", "-c", config], output, "nginx-config") as check:
                check.wait(10)
            nginx = stack.enter_context(Process([args.nginx, "-p", str(output) + "/", "-c", config], output, "nginx"))
            wait_http(f"http://127.0.0.1:{edge_port}/health", nginx)

            healthy, _ = observe("healthy-playlist", "/healthy/index.m3u8")
            healthy_segment, _ = observe("healthy-segment", "/healthy/segment-0.ts")
            assert healthy["status"] == healthy_segment["status"] == 200
            assert healthy["headers"].get("Content-Type") == "application/vnd.apple.mpegurl"
            assert healthy_segment["headers"].get("Content-Type") == "video/mp2t"
            with Process([args.ffmpeg, "-hide_banner", "-v", "error", "-i",
                          output / "responses" / "healthy-segment.body", "-f", "null", "-"], output, "decode") as decode:
                decode.wait(15)
            if (output / "decode.log").stat().st_size:
                raise RuntimeError("normal fragment emitted decode errors")

            playlist(media / "missing" / "index.m3u8", 0, "pending.ts")
            published, pending_list = observe("missing-published-list", "/missing/index.m3u8")
            assert published["status"] == 200 and b"pending.ts" in pending_list
            missing, _ = observe("missing-before-asset", "/missing/pending.ts")
            shutil.copyfile(segment0, media / "missing" / "pending.ts")
            available, _ = observe("missing-after-asset", "/missing/pending.ts")
            assert missing["status"] == 404 and available["status"] == 200

            first, old_list = observe("stale-first", "/stale/index.m3u8")
            playlist(media / "stale" / "index.m3u8", 1, "segment-1.ts")
            _, current_list = observe("stale-origin-current", "/stale/index.m3u8", origin=True)
            cached, cached_list = observe("stale-edge-cached", "/stale/index.m3u8")
            assert first["headers"].get("X-Lab-Cache") == "MISS"
            assert cached["headers"].get("X-Lab-Cache") == "HIT" and cached_list == old_list != current_list
            time.sleep(6)
            expired, renewed_list = observe("stale-expired", "/stale/index.m3u8")
            assert renewed_list == current_list and expired["headers"].get("X-Lab-Cache") == "EXPIRED"

            _, retained_list = observe("retire-client-list", "/retire/index.m3u8")
            playlist(media / "retire" / "index.m3u8", 1, "segment-1.ts")
            (media / "retire" / "segment-0.ts").unlink()
            removed, _ = observe("retire-client-old-reference", "/retire/segment-0.ts")
            assert b"segment-0.ts" in retained_list and removed["status"] == 404
            observe("retire-current-list", "/retire/index.m3u8")

            headers = {"X-Lab-Token": token(int(time.time()) + 3)}
            granted, _ = observe("auth-first", "/guarded/segment-0.ts", headers)
            hit, _ = observe("auth-hit", "/guarded/segment-0.ts", headers)
            assert granted["headers"].get("X-Lab-Cache") == "MISS"
            assert hit["headers"].get("X-Lab-Cache") == "HIT"
            assert hit["origin_resource_requests"] == 1 and hit["authorization_requests"] == 2
            time.sleep(3.1)
            denied, _ = observe("auth-expired", "/guarded/segment-0.ts", headers)
            assert denied["status"] == 401 and denied["origin_resource_requests"] == 1
            headers = {"X-Lab-Token": token(int(time.time()) + 60)}
            fresh, _ = observe("auth-renewed-hit", "/guarded/segment-0.ts", headers)
            assert fresh["headers"].get("X-Lab-Cache") == "HIT" and fresh["authorization_requests"] == 4
            assert fresh["origin_resource_requests"] == 1
            if nginx.process.poll() is not None:
                raise RuntimeError("Nginx exited before experiment cleanup")
    finally:
        if server:
            server.shutdown()
            server.server_close()
    exits = {entry["name"]: entry for entry in
             (json.loads(line) for line in (output / "commands.jsonl").read_text().splitlines())
             if "exit_code" in entry}
    if any(exits[name]["exit_code"] != 0 for name in ("generate", "decode", "nginx-config", "nginx")):
        raise RuntimeError("experiment process exited unsuccessfully; inspect commands.jsonl")
    if not exits["nginx"]["requested_stop"]:
        raise RuntimeError("Nginx exited without the experiment requesting cleanup")
    for name in ("generate", "decode", "nginx"):
        if (output / f"{name}.log").read_text(errors="replace").strip():
            raise RuntimeError(f"unexpected {name} output; inspect its log")
    expected_config = [f"nginx: the configuration file {config} syntax is ok",
                       f"nginx: configuration file {config} test is successful"]
    if (output / "nginx-config.log").read_text().splitlines() != expected_config:
        raise RuntimeError("unexpected Nginx configuration check output")
    nginx_log = (output / "nginx-error.log").read_text(errors="replace").splitlines()
    if not nginx_log or any("[notice]" not in line for line in nginx_log):
        raise RuntimeError("unexpected Nginx runtime diagnostic; inspect nginx-error.log")
    check_ports((origin_port, edge_port))
    write_json(output / "result.json", dict(passed=True, observations=observations,
        expected_error_statuses={"missing-before-asset": 404, "retire-client-old-reference": 404,
                                 "auth-expired": 401},
        cleanup=dict(process_exit_codes={name: entry["exit_code"] for name, entry in exits.items()},
                     nginx_stop_requested=True, ports_released=True),
        log_audit=dict(unexpected_diagnostics=0, nginx_notices=len(nginx_log)),
        authorization="local signed expiry policy; ChatServer business authorization belongs to lifecycle lesson"))
    print("All publication, retention, cache and auth observations matched; cleanup and logs checked.", flush=True)


if __name__ == "__main__":
    main()
