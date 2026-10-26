"""生成阶段 1～3 共用的真实媒体；只写入尚不存在的输出目录。"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    commands = []

    def generate(name, options):
        command = [args.ffmpeg, "-hide_banner", "-v", "error", "-nostdin", "-n",
                   *options, str(output / name)]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8")
        commands.append({"name": name, "command": command, "stderr": result.stderr})
        (output / "commands.json").write_text(json.dumps(commands, indent=2, ensure_ascii=False), encoding="utf-8")
        if result.returncode or result.stderr:
            raise RuntimeError(f"{name}: exit={result.returncode}\n{result.stderr}")
        return str(output / name)

    # 四角色块提供不依赖文字或字体的方向基准；中间 testsrc2 保留运动内容。
    pattern = ("testsrc2=size=854x480:rate=30:duration=8,"
               "drawbox=x=0:y=0:w=160:h=120:color=red:t=fill,"
               "drawbox=x=iw-160:y=0:w=160:h=120:color=lime:t=fill,"
               "drawbox=x=0:y=ih-120:w=160:h=120:color=blue:t=fill,"
               "drawbox=x=iw-160:y=ih-120:w=160:h=120:color=yellow:t=fill")
    video = ["-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-pix_fmt", "yuv420p",
             "-g", "60", "-bf", "2", "-sc_threshold", "0", "-color_range", "tv",
             "-colorspace", "bt709", "-color_trc", "bt709", "-color_primaries", "bt709"]
    base = generate("base.mp4", ["-f", "lavfi", "-i", pattern, "-f", "lavfi", "-i",
                                 "sine=frequency=440:sample_rate=48000:duration=8", *video,
                                 "-c:a", "aac", "-b:a", "128k", "-ac", "2", "-movflags", "+faststart"])
    generate("base.mkv", ["-i", base, "-map", "0", "-c", "copy"])
    sar = generate("sar_4_3.mp4", ["-i", base, "-vf", "setsar=4/3", *video, "-c:a", "copy"])
    for degrees in (90, 180, 270, 45):
        generate(f"rotate_{degrees}.mp4", ["-display_rotation", str(degrees), "-i", base,
                                           "-map", "0", "-c", "copy"])
    generate("rotate_90_sar.mp4", ["-display_rotation", "90", "-i", sar, "-map", "0", "-c", "copy"])
    generate("mirror.mp4", ["-display_hflip", "-i", base, "-map", "0", "-c", "copy"])
    generate("audio_late.mp4", ["-i", base, "-itsoffset", "0.75", "-i", base,
                                 "-map", "0:v:0", "-map", "1:a:0", "-c", "copy", "-t", "8"])
    generate("audio_gap.mkv", ["-i", base, "-c:v", "copy", "-af", "aselect=not(between(t\\,2\\,3))",
                                "-c:a", "aac", "-b:a", "128k"])
    generate("vfr_bframes.mp4", ["-i", base, "-vf", "select=not(mod(n\\,3))+gte(t\\,4)",
                                   "-fps_mode", "vfr", *video, "-c:a", "copy"])
    manifest = {"corner_colors_untransformed": ["red", "lime", "blue", "yellow"],
                "corner_order": ["top_left", "top_right", "bottom_left", "bottom_right"],
                "rotation_semantics": "FFmpeg display_rotation: counter-clockwise degrees",
                "files": []}
    for path in sorted(output.iterdir()):
        if path.suffix not in (".mp4", ".mkv"):
            continue
        command = [args.ffprobe, "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", check=True)
        if result.stderr:
            raise RuntimeError(result.stderr)
        manifest["files"].append({"name": path.name, "bytes": path.stat().st_size,
                                  "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                                  "probe": json.loads(result.stdout)})
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"generated={len(manifest['files'])} output={output}")


if __name__ == "__main__":
    main()
