"""运行受控转封装、编码与分片对照，并保留真实命令和观察数据。"""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("samples", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--media-process", required=True)
    parser.add_argument("--media-info", required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()
    source = args.samples.resolve() / "base.mp4"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []

    def run(command, expected_error=None):
        command = list(map(str, command))
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8")
        records.append({"command": command, "elapsed_s": time.monotonic() - started,
                        "exit": result.returncode, "stdout": result.stdout, "stderr": result.stderr,
                        "expected_error": expected_error})
        (output / "commands.json").write_text(json.dumps(records, indent=2, ensure_ascii=False), encoding="utf-8")
        if expected_error:
            if result.returncode == 0 or expected_error not in result.stderr:
                raise RuntimeError(f"expected {expected_error!r}, got {result.stderr}")
            unexpected = [line for line in result.stderr.splitlines() if line and expected_error not in line]
            if unexpected:
                raise RuntimeError(f"unexpected error output: {unexpected}")
        elif result.returncode or result.stderr:
            raise RuntimeError(f"exit={result.returncode} stderr={result.stderr}")
        return result.stdout

    def probe(path, mode):
        return json.loads(run([args.ffprobe, "-v", "error", f"-show_{mode}", "-of", "json", path]))[mode]

    def decode(path):
        run([args.ffmpeg, "-hide_banner", "-v", "error", "-xerror", "-i", path, "-map", "0:v:0",
             "-map", "0:a?", "-f", "null", "-"])

    def frame_hashes(path):
        text = run([args.ffmpeg, "-hide_banner", "-v", "error", "-xerror", "-i", path,
                    "-map", "0:v:0", "-fps_mode", "passthrough", "-f", "framemd5", "-"])
        return [line.rsplit(",", 1)[1].strip() for line in text.splitlines() if line and not line.startswith("#")]

    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    for name in ("base.mp4", "sar_4_3.mp4", "rotate_90_sar.mp4", "mirror.mp4", "rotate_45.mp4"):
        run([args.media_info, args.samples / name, "1"])
    ts = output / "remux.ts"
    run([args.media_process, "remux", source, ts])
    run([args.media_process, "remux", source, output / "remux.mkv"])
    run([args.media_process, "transcode", source, output / "gop1-b0",
         "--gop-seconds", "1", "--b-frames", "0", "--crf", "23", "--max-rate-kbps", "900"])
    run([args.media_process, "transcode", source, output / "gop3-b2",
         "--gop-seconds", "3", "--b-frames", "2", "--crf", "23", "--max-rate-kbps", "900"])
    for container in ("ts", "fmp4"):
        run([args.media_process, "hls", source, output / f"hls-{container}", container])
    for path in (ts, output / "remux.mkv", *output.glob("gop*/*.mp4")):
        decode(path)
    original_frames = frame_hashes(source)
    assert frame_hashes(ts) == original_frames, "TS remux changed decoded video pixels"
    assert frame_hashes(output / "remux.mkv") == original_frames, "MKV remux changed decoded video pixels"

    # TS 内部的数据已经是 Annex B；读取真实 packet 数据，不额外调用 BSF 生成证据。
    packet_dump = json.loads(run([args.ffprobe, "-v", "error", "-select_streams", "v:0", "-read_intervals", "%+#1",
                                 "-show_packets", "-show_data", "-of", "json", ts]))["packets"][0]
    data = b"".join(bytes.fromhex(line.split(":", 1)[1].split("  ", 1)[0].strip())
                     for line in packet_dump["data"].strip().splitlines())
    nals = [block[0] & 31 for block in re.split(b"\x00\x00\x00?\x01", data) if block]
    assert 7 in nals and 8 in nals and 5 in nals, f"expected SPS/PPS/IDR, got {nals}"

    summary = {"source_sha256": source_hash, "remux_video_frames_identical": len(original_frames),
               "ts_first_video_packet_nal_types": nals, "encodes": {}, "hls": {}}
    for folder in ("gop1-b0", "gop3-b2"):
        path = output / folder / "480p.mp4"
        frames = [frame for frame in probe(path, "frames") if frame["media_type"] == "video"]
        types = Counter(frame["pict_type"] for frame in frames)
        keys = [float(frame["best_effort_timestamp_time"]) for frame in frames if frame["key_frame"]]
        packets = [packet for packet in probe(path, "packets") if packet["stream_index"] == 0]
        dts = [int(packet["dts"]) for packet in packets]
        assert len(frames) == len(original_frames) and all(b > a for a, b in zip(dts, dts[1:]))
        assert (types["B"] == 0) if folder == "gop1-b0" else (types["B"] > 0)
        summary["encodes"][folder] = {"picture_types": dict(types), "keyframe_pts_s": keys,
                                      "bytes": path.stat().st_size,
                                      "first_packets": packets[:6]}
    for container in ("ts", "fmp4"):
        root = output / f"hls-{container}"
        catalog = json.loads((root / "catalog.json").read_text(encoding="utf-8"))
        variants = []
        for variant in catalog["variants"]:
            playlist = root / variant["playlist"]
            decode(playlist)
            lines = playlist.read_text(encoding="utf-8").splitlines()
            segments = [line for line in lines if line and not line.startswith("#")]
            # fMP4 单片必须和同一档 init.mp4 一起送入解封装器。
            for segment in (segments[0], segments[-1]):
                path = playlist.parent / segment
                if container == "fmp4":
                    path = output / f"{variant['id']}-{segment}.mp4"
                    path.write_bytes((playlist.parent / "init.mp4").read_bytes() + (playlist.parent / segment).read_bytes())
                decode(path)
            variants.append({"id": variant["id"], "segments": segments,
                             "durations": [float(line[8:].split(",")[0]) for line in lines if line.startswith("#EXTINF:")]})
        summary["hls"][container] = variants
    run([args.media_process, "remux", source, ts], "output or .part file already exists")
    run([args.media_process, "hls", source, output / "invalid-hls", "--gop-seconds", "3"],
        "hls accepts only an optional segment type")
    run([args.media_process, "transcode", source, output / "invalid-bframes", "--b-frames", "9"],
        "transcode options exceed the lesson limits")
    run([args.media_process, "transcode", source, output / "invalid-lossless", "--crf", "0"],
        "transcode options exceed the lesson limits")
    pcm = output / "pcm-audio.mkv"
    run([args.ffmpeg, "-hide_banner", "-v", "error", "-nostdin", "-n", "-i", source,
         "-c:v", "copy", "-c:a", "pcm_s16le", pcm])
    run([args.media_process, "remux", pcm, output / "invalid-codec.ts"],
        "TS lesson accepts H.264 video and optional AAC audio only")
    assert hashlib.sha256(source.read_bytes()).hexdigest() == source_hash
    assert not list(output.rglob("*.part")), "unexpected unfinished output"
    (output / "observations.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"completed output={output} remux_video_frames={len(original_frames)}")


if __name__ == "__main__":
    main()
