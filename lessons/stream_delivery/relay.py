"""Forward one loopback HTTP-FLV source with a bounded, per-viewer read rate."""
import argparse
import http.client
import http.server
import json
from pathlib import Path
import socket
import threading
import time
from urllib.parse import urlsplit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--origin", required=True, help="fixed http://127.0.0.1:port/app/stream.flv")
    parser.add_argument("--port", type=int, default=21981)
    parser.add_argument("--slow-bytes-per-second", type=int, default=32768)
    parser.add_argument("--slow-seconds", type=float, default=25)
    parser.add_argument("--events", type=Path, required=True)
    args = parser.parse_args()
    origin = urlsplit(args.origin)
    if origin.scheme != "http" or origin.hostname != "127.0.0.1" or origin.username or origin.query:
        parser.error("origin must be a fixed loopback HTTP path without credentials/query")
    if args.slow_bytes_per_second <= 0 or args.slow_seconds < 0:
        parser.error("rate must be positive and duration nonnegative")
    lock = threading.Lock()

    def record(value):
        with lock, args.events.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(value) + "\n")

    class Relay(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            if self.path == "/health":
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"ok")
                return
            if self.path not in ("/normal", "/slow"):
                self.send_error(404)
                return
            name = self.path[1:]
            started = time.monotonic()
            sent = 0
            received = 0
            last_report = 0.0
            next_read = started
            upstream = http.client.HTTPConnection(origin.hostname, origin.port or 80, timeout=5)
            response = None
            outcome = "eof"
            try:
                upstream.connect()
                # Fixed bounded capacity permits recovery without depending on TCP window resizing.
                upstream.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 262144)
                self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 262144)
                self.connection.settimeout(5)
                record(dict(event="connected", viewer=name, unix_ms=int(time.time() * 1000),
                            upstream_local=upstream.sock.getsockname(),
                            receive_buffer=upstream.sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF),
                            send_buffer=self.connection.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF),
                            chunk_bytes=4096))
                upstream.request("GET", origin.path, headers={"Connection": "close"})
                response = upstream.getresponse()
                self.send_response(response.status)
                self.send_header("Content-Type", response.getheader("Content-Type", "application/octet-stream"))
                self.send_header("Connection", "close")
                self.end_headers()
                released = name != "slow"
                while True:
                    now = time.monotonic()
                    limited = name == "slow" and now - started < args.slow_seconds
                    if not limited and not released:
                        record(dict(event="released", viewer=name, elapsed_s=round(now - started, 3)))
                        released = True
                    if limited and next_read > now:
                        time.sleep(min(next_read - now, args.slow_seconds - (now - started)))
                    # There is no producer thread or accumulating application queue.
                    chunk = response.read1(4096)
                    if not chunk:
                        break
                    received += len(chunk)
                    self.wfile.write(chunk)
                    sent += len(chunk)
                    now = time.monotonic()
                    if limited:
                        next_read = max(next_read, now) + len(chunk) / args.slow_bytes_per_second
                    if now - last_report >= 1:
                        record(dict(event="progress", viewer=name, elapsed_s=round(now - started, 3),
                                    received=received, sent=sent, limited=limited))
                        last_report = now
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                outcome = "viewer_closed"
            except (OSError, http.client.HTTPException) as error:
                outcome = type(error).__name__
            finally:
                if response is not None:
                    response.close()
                upstream.close()
                self.close_connection = True
                record(dict(event="closed", viewer=name, elapsed_s=round(time.monotonic() - started, 3),
                            received=received, sent=sent, outcome=outcome))

        def log_message(self, *unused):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Relay)
    try:
        server.serve_forever()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
