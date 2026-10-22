"""Run the real executables over loopback UDP and interoperate with FFmpeg."""
import argparse
import os
from pathlib import Path
import re
import socket
import subprocess
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def ports():
    """Reserve/check four adjacent UDP ports, then return them to the real peers."""
    for base in range(24000, 26000, 4):
        bound = []
        try:
            for number in range(base, base + 4):
                handle = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                bound.append(handle)
                handle.bind(("127.0.0.1", number))
            return base
        except OSError:
            pass
        finally:
            for handle in bound:
                handle.close()
    raise RuntimeError("no free UDP port group")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--sample", type=Path, required=True)
    parser.add_argument("--ffmpeg", type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    output = build / "verification"
    output.mkdir(exist_ok=True)
    ffmpeg = str(args.ffmpeg.resolve())
    sample = str(args.sample.resolve())
    environment = os.environ.copy()
    environment["QT_QPA_PLATFORM"] = "offscreen"
    if os.name == "nt":
        environment["QT_QPA_FONTDIR"] = "C:/Windows/Fonts"
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0

    def run(command, name, timeout=20, binary=False):
        result = subprocess.run(command, capture_output=True, env=environment,
                                creationflags=flags, timeout=timeout)
        (output / (name + ".stderr.log")).write_bytes(result.stderr)
        if not binary:
            (output / (name + ".log")).write_bytes(result.stdout)
        require(result.returncode == 0, f"{name} failed: {result.stderr.decode(errors='replace')}")
        clean(result.stderr.decode(errors="replace"), name)
        return result.stdout

    def clean(text, name):
        require(not re.search(r"failed|error|warning|\[h264", text, re.I),
                f"unexpected diagnostics in {name}: {text}")

    def start(command, name):
        stream = (output / (name + ".log")).open("wb")
        process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT,
                                   env=environment, creationflags=flags)
        return process, stream, name

    def finish(child, timeout=15):
        process, stream, name = child
        code = process.wait(timeout=timeout)
        stream.close()
        text = (output / (name + ".log")).read_text(encoding="utf-8", errors="replace")
        require(code == 0, f"{name} exited {code}: {text}")
        clean(text, name)
        return text

    def ready(child, port=None):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            process, _, name = child
            require(process.poll() is None, f"{name} exited before readiness")
            if port is None:
                if "receiver listening=" in (output / (name + ".log")).read_text(errors="replace"):
                    return
            else:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                    try:
                        probe.bind(("127.0.0.1", port))
                    except OSError:
                        return
            time.sleep(0.05)
        raise RuntimeError(f"{child[2]} did not become ready")

    def values(text):
        line = re.search(r"receiver summary ([^\r\n]+)", text)
        require(line is not None, "receiver summary missing")
        return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line[1])}

    sender = str(build / "rtp_sender.exe")
    receiver = str(build / "rtp_receiver.exe")
    run([str(build / "protocol_check.exe")], "protocol")
    results = []
    for name, impairments in [("normal", []), ("loss-reorder", ["--drop-every", "97", "--reorder-every", "7"])]:
        base = ports()
        child = start([receiver, "--port", str(base), "--seconds", "7",
                       "--save-first-frame", str(output / (name + ".png")),
                       "--screenshot", str(output / (name + "-window.png"))], name + "-receiver")
        ready(child)
        sent = run([sender, sample, "--port", str(base), "--local-port", str(base + 2)] + impairments,
                   name + "-sender").decode()
        received = finish(child)
        summary = values(received)
        require(summary["decoded"] > 25 and summary["presented"] > 20, f"{name}: insufficient playback")
        require(summary["sr"] >= 2 and summary["rr"] >= 2 and "RTCP RR lost=" in sent,
                f"{name}: SR/RR not exchanged")
        require(summary["invalid"] == 0, f"{name}: invalid datagrams")
        if not impairments:
            require(summary["gaps"] == 0 and summary["damaged_units"] == 0 and summary["decoded"] == 125,
                    f"normal loss or missing decoded frames: {summary}")
        else:
            require(summary["gaps"] > 0 and summary["damaged_units"] > 0 and summary["decoded"] < 125,
                    "loss did not exercise discard/recovery")
            require(re.search(r"RTCP RR lost=[1-9]", sent), "RR did not report injected loss")
            require("RTP recovered at IDR" in received, "no decoded output recovered after a confirmed gap")
        results.append(f"{name}: {summary}")

    # 比较实际接收解码出的首帧，不以收到报文或窗口创建成功代替画面验证。
    reference = run([ffmpeg, "-v", "error", "-sws_flags", "bilinear", "-i", sample,
                     "-frames:v", "1", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"], "reference-rgb", binary=True)
    actual = run([ffmpeg, "-v", "error", "-i", str(output / "normal.png"), "-frames:v", "1",
                  "-pix_fmt", "rgb24", "-f", "rawvideo", "-"], "received-rgb", binary=True)
    require(len(reference) == len(actual), "RGB dimensions differ")
    difference = sum(abs(a - b) for a, b in zip(reference, actual)) / len(reference)
    require(difference < 1.0, f"decoded RGB differs from FFmpeg: mean error {difference}")
    results.append(f"first-frame RGB mean absolute error: {difference:.6f}")

    # 外部 FFmpeg 发送端检验 STAP-A/FU-A 接收，而不是只让自有程序互发。
    base = ports()
    child = start([receiver, "--port", str(base), "--seconds", "7"], "ffmpeg-to-qt-receiver")
    ready(child)
    run([ffmpeg, "-v", "error", "-re", "-i", sample, "-map", "0:v:0", "-an", "-c:v", "copy",
         "-bsf:v", "h264_mp4toannexb", "-payload_type", "96", "-f", "rtp", "-pkt_size", "1200",
         f"rtp://127.0.0.1:{base}?rtcpport={base + 1}"], "ffmpeg-sender")
    summary = values(finish(child))
    require(summary["decoded"] == 125 and summary["gaps"] == 0 and summary["invalid"] == 0
            and summary["sr"] >= 1 and summary["rr"] >= 1,
            f"FFmpeg-to-Qt mismatch: {summary}")
    results.append(f"FFmpeg-to-Qt: {summary}")

    # 外部 FFmpeg 接收端通过 SDP 获知 PT=96 与 H.264 的映射。
    base = ports()
    sdp = output / "receive.sdp"
    sdp.write_text("v=0\no=- 0 0 IN IP4 127.0.0.1\ns=RTP lesson\nc=IN IP4 127.0.0.1\nt=0 0\n"
                   f"m=video {base} RTP/AVP 96\na=rtpmap:96 H264/90000\na=fmtp:96 packetization-mode=1\n",
                   encoding="ascii")
    network_hashes = output / "network.framemd5"
    child = start([ffmpeg, "-v", "error", "-y", "-protocol_whitelist", "file,udp,rtp",
                   "-listen_timeout", "10", "-i", str(sdp), "-map", "0:v:0", "-frames:v", "125",
                   "-fps_mode", "passthrough", "-pix_fmt", "yuv420p", "-f", "framemd5", str(network_hashes)],
                  "qt-to-ffmpeg-receiver")
    ready(child, base)
    run([sender, sample, "--port", str(base), "--local-port", str(base + 2)], "qt-to-ffmpeg-sender")
    finish(child)
    reference_hashes = output / "reference.framemd5"
    run([ffmpeg, "-v", "error", "-y", "-i", sample, "-map", "0:v:0", "-fps_mode", "passthrough",
         "-pix_fmt", "yuv420p", "-f", "framemd5", str(reference_hashes)], "reference-frames")

    def hashes(path):
        return [line.split(",")[-1].strip() for line in path.read_text().splitlines()
                if line and not line.startswith("#")]

    require(hashes(network_hashes) == hashes(reference_hashes) and len(hashes(network_hashes)) == 125,
            "RTP sender decoded frame hashes differ from local file")
    results.append("Qt-to-FFmpeg: all 125 decoded YUV frame hashes equal local reference")
    report = "\n".join(results) + "\n"
    (output / "summary.txt").write_text(report, encoding="utf-8")
    print(report)


if __name__ == "__main__":
    main()
