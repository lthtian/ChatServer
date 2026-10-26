"""Run a real SRS stream with normal and throttled HTTP-FLV viewers (Linux/WSL)."""
import argparse
import contextlib
import http.client
import json
from pathlib import Path
import re
import subprocess
import sys
import threading
import time

from common import Process, append_json, check_ports, output_directory, request, wait_http, write_json


def audit_logs(output):
    for name in ("generate", "publisher", "relay"):
        if (output / f"{name}.log").read_text(errors="replace").strip():
            raise RuntimeError(f"unexpected {name} output; inspect its log")
    text = (output / "srs.log").read_text(errors="replace")
    lines = text.splitlines()
    shutdown = max(index for index, line in enumerate(lines) if "/api/v1/clients" in line)
    expected = []
    for index, line in enumerate(lines):
        if "[ERROR]" in line:
            raise RuntimeError(f"SRS error: {line}")
        if "[WARN]" not in line:
            continue
        ordinary = ("stats network use index=", "stats disk not configed", "client disconnect peer. ret=1007",
                    "client disconnect peer. ret=1009", "main cycle terminated, system quit normally.",
                    "dispose unlink path failed. file=", "ignore remove mpd failed, /")
        if any(value in line for value in ordinary):
            expected.append(line)
        elif index > shutdown and "VIDEO: Timestamp " in line:
            expected.append(line)
        else:
            raise RuntimeError(f"unclassified SRS warning: {line}")
    write_json(output / "expected-srs-warnings.json", expected)
    return dict(unexpected_errors=0, expected_srs_warnings=len(expected),
                note="startup statistics and owned connection/service teardown; timestamp flush only after viewer release")


class Viewer(threading.Thread):
    """Observe FLV tags without accumulating media payloads or decoding fake frames."""
    def __init__(self, name, port, output):
        super().__init__(daemon=True)
        self.name = name
        self.port = port
        self.output = output
        self.cancel = threading.Event()
        self.connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        self.stats = dict(viewer=name, bytes=0, video_tags=0, keyframes=0, configuration_tags=0,
                          first_dts_ms=None, last_dts_ms=None, largest_video_gap_ms=0,
                          backward_dts=0, error=None)
        self.last_report = 0

    def exact(self, response, size):
        data = bytearray()
        while len(data) < size:
            if self.cancel.is_set():
                raise InterruptedError()
            chunk = response.read(size - len(data))
            if not chunk:
                raise EOFError("FLV response ended")
            self.stats["bytes"] += len(chunk)
            data.extend(chunk)
        return bytes(data)

    def run(self):
        self.started = time.monotonic()
        try:
            self.connection.request("GET", f"/{self.name}")
            response = self.connection.getresponse()
            if response.status != 200:
                raise RuntimeError(f"HTTP {response.status}")
            header = self.exact(response, 9)
            if header[:3] != b"FLV" or int.from_bytes(header[5:9], "big") != 9:
                raise ValueError("expected ordinary FLV header")
            self.exact(response, 4)
            while not self.cancel.is_set():
                tag = self.exact(response, 11)
                size = int.from_bytes(tag[1:4], "big")
                if size > 16 * 1024 * 1024:
                    raise ValueError("FLV tag exceeds lesson observation limit")
                timestamp = int.from_bytes(tag[4:7], "big") | (tag[7] << 24)
                prefix = self.exact(response, min(size, 5))
                remaining = size - len(prefix)
                while remaining:
                    count = min(remaining, 4096)
                    self.exact(response, count)
                    remaining -= count
                if int.from_bytes(self.exact(response, 4), "big") != size + 11:
                    raise ValueError("FLV PreviousTagSize mismatch")
                if tag[0] == 9 and len(prefix) >= 2:
                    if prefix[1] == 0:
                        self.stats["configuration_tags"] += 1
                    elif prefix[1] == 1:
                        previous = self.stats["last_dts_ms"]
                        if previous is None:
                            self.stats["first_dts_ms"] = timestamp
                            self.stats["first_video_after_s"] = round(time.monotonic() - self.started, 3)
                        else:
                            self.stats["largest_video_gap_ms"] = max(self.stats["largest_video_gap_ms"], timestamp - previous)
                            self.stats["backward_dts"] += int(timestamp < previous)
                        self.stats["last_dts_ms"] = timestamp
                        self.stats["video_tags"] += 1
                        self.stats["keyframes"] += int(prefix[0] >> 4 == 1)
                now = time.monotonic()
                if now - self.last_report >= 1:
                    append_json(self.output / f"{self.name}.jsonl", dict(self.stats, elapsed_s=round(now - self.started, 3)))
                    self.last_report = now
        except (InterruptedError, OSError) as error:
            if not self.cancel.is_set():
                self.stats["error"] = str(error)
        except Exception as error:
            self.stats["error"] = str(error)
        finally:
            self.connection.close()
            append_json(self.output / f"{self.name}.jsonl", dict(self.stats, event="closed"))

    def close(self):
        self.cancel.set()
        self.connection.close()
        self.join(timeout=6)
        if self.is_alive():
            raise RuntimeError(f"{self.name} viewer failed to stop")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--srs", default="/mnt/d/chat/_server/Output/linux-x64/rtc/srs")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--output")
    parser.add_argument("--seconds", type=int, default=45)
    parser.add_argument("--slow-seconds", type=int, default=25)
    parser.add_argument("--base-port", type=int, default=21980)
    args = parser.parse_args()
    if args.slow_seconds < 10 or args.seconds < args.slow_seconds + 10:
        parser.error("allow at least 10s throttling and 10s recovery")
    http, relay_port, api, rtmp = args.base_port, args.base_port + 1, args.base_port + 5, args.base_port + 6
    check_ports((http, relay_port, api, rtmp))
    output = output_directory(args.output, "delivery")
    print(f"output: {output}", flush=True)
    version = subprocess.check_output([args.srs, "-v"], stderr=subprocess.STDOUT, text=True).strip()
    if version != "6.0.184":
        raise RuntimeError(f"expected fixed SRS 6.0.184, found {version}")
    config = output / "srs.conf"
    config.write_text(f"""listen 127.0.0.1:{rtmp};
max_connections 16;
daemon off;
pid {output}/srs.pid;
srs_log_tank console;
srs_log_level trace;
http_api {{ enabled on; listen 127.0.0.1:{api}; }}
http_server {{ enabled on; listen 127.0.0.1:{http}; dir {output}; }}
vhost __defaultVhost__ {{
    play {{ gop_cache on; queue_length 2; }}
    http_remux {{ enabled on; mount [vhost]/[app]/[stream].flv; }}
}}
""", encoding="utf-8")
    sample = output / "source.mp4"
    with contextlib.ExitStack() as stack:
        generate = stack.enter_context(Process([args.ffmpeg, "-hide_banner", "-loglevel", "warning",
            "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30", "-f", "lavfi", "-i",
            "sine=frequency=440:sample_rate=48000", "-t", "8", "-c:v", "libx264", "-threads", "2",
            "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-b:v", "8M", "-minrate", "8M", "-maxrate", "8M",
            "-bufsize", "8M", "-g", "30", "-keyint_min", "30", "-sc_threshold", "0", "-bf", "0",
            "-x264-params", "nal-hrd=cbr", "-c:a", "aac", "-b:a", "128k", "-y", sample], output, "generate"))
        generate.wait(60)
        srs = stack.enter_context(Process([args.srs, "-c", config], output, "srs", cwd=output))
        wait_http(f"http://127.0.0.1:{api}/api/v1/versions", srs)
        publisher = stack.enter_context(Process([args.ffmpeg, "-hide_banner", "-loglevel", "warning", "-re",
            "-stream_loop", "-1", "-i", sample, "-c", "copy", "-f", "flv", "-flvflags", "no_duration_filesize",
            f"rtmp://127.0.0.1:{rtmp}/live/lesson"], output, "publisher"))
        deadline = time.monotonic() + 10
        while True:
            _, body = request(f"http://127.0.0.1:{api}/api/v1/streams")
            streams = json.loads(body)["streams"]
            if streams and streams[0].get("video"):
                write_json(output / "stream-start.json", streams)
                break
            if publisher.process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("publisher did not deliver video")
            time.sleep(.1)
        relay = stack.enter_context(Process([sys.executable, Path(__file__).with_name("relay.py"),
            "--origin", f"http://127.0.0.1:{http}/live/lesson.flv", "--port", relay_port,
            "--slow-seconds", args.slow_seconds, "--events", output / "relay.jsonl"], output, "relay"))
        wait_http(f"http://127.0.0.1:{relay_port}/health", relay)
        viewers = [Viewer(name, relay_port, output) for name in ("normal", "slow")]
        try:
            for viewer in viewers:
                viewer.start()
            started = time.monotonic()
            while time.monotonic() - started < args.seconds:
                _, body = request(f"http://127.0.0.1:{api}/api/v1/clients")
                rss = re.search(r"VmRSS:\s+(\d+)", Path(f"/proc/{srs.process.pid}/status").read_text())
                append_json(output / "server.jsonl", dict(elapsed_s=round(time.monotonic() - started, 3),
                    rss_kib=int(rss.group(1)), clients=json.loads(body)["clients"]))
                sockets = subprocess.check_output(["ss", "-tinm", f"sport = :{http}"], text=True)
                with (output / "sockets.txt").open("a", encoding="utf-8") as stream:
                    stream.write(f"elapsed_s={time.monotonic() - started:.3f}\n{sockets}\n")
                if any(viewer.stats["error"] for viewer in viewers):
                    raise RuntimeError("viewer failed; inspect viewer logs")
                time.sleep(1)
        finally:
            for viewer in viewers:
                viewer.close()
        deadline = time.monotonic() + 8
        while True:
            _, body = request(f"http://127.0.0.1:{api}/api/v1/clients")
            remaining = json.loads(body)
            if len(remaining["clients"]) == 1 and remaining["clients"][0]["publish"]:
                break
            if time.monotonic() > deadline:
                write_json(output / "clients-after-viewers.json", remaining)
                raise RuntimeError("viewer connections remained after cancellation")
            time.sleep(.2)
        write_json(output / "clients-after-viewers.json", remaining)
        summary = dict(srs_version=version, source_bitrate_bps=8000000, slow_rate_bytes_s=32768,
                       slow_seconds=args.slow_seconds, seconds=args.seconds,
                       viewers=[viewer.stats for viewer in viewers],
                       shrink_events=(output / "srs.log").read_text(errors="replace").count("shrinking,"))
        write_json(output / "result.json", summary)
        normal, slow = (viewer.stats for viewer in viewers)
        if normal["video_tags"] < args.seconds * 25 or normal["largest_video_gap_ms"] > 100 or normal["backward_dts"]:
            raise RuntimeError("normal viewer continuity failed")
        if summary["shrink_events"] == 0 or slow["largest_video_gap_ms"] < 1000:
            raise RuntimeError("backpressure/queue-shrink evidence not established; inspect sockets and rates")
        if slow["last_dts_ms"] < normal["last_dts_ms"] - 1500:
            raise RuntimeError("slow viewer did not recover near current media position")
    summary["log_audit"] = audit_logs(output)
    summary["passed"] = True
    write_json(output / "result.json", summary)
    print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    main()
