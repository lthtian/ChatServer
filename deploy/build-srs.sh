#!/usr/bin/env bash
# 固定源码在本机编译；不调用远程编译器，不构建本阶段无关协议。
set -euo pipefail
source=/opt/chat-build/deps/srs-v6.0-r0/trunk
output=${1:-/mnt/d/chat/_server/Output/linux-x64/Release}
patch_file=$(cd "$(dirname "$0")" && pwd)/srs-6.0.184-sender-report.patch
test -f "$source/configure"
cd "$source"
if patch --dry-run --batch -p1 < "$patch_file" >/dev/null 2>&1; then
    patch --batch -p1 < "$patch_file"
elif ! patch --dry-run --batch -R -p1 < "$patch_file" >/dev/null 2>&1; then
    echo "SRS source does not match the sender-report patch" >&2
    exit 1
fi
if [[ ! -f objs/Makefile ]] || ! grep -q '^#define SRS_RTC$' objs/srs_auto_headers.hpp; then
    ./configure --rtc=on --ffmpeg-fit=off --srt=off --https=off --h265=off --apm=off --sanitizer=off --backtrace=off --jobs=2
fi
timeout 600 make -j2
mkdir -p "$output"
cp objs/srs "$output/srs"
file "$output/srs"
sha256sum "$output/srs"
