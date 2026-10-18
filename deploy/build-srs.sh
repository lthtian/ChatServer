#!/usr/bin/env bash
# 固定源码在本机编译；不调用远程编译器，不构建本阶段无关协议。
set -euo pipefail
source=/opt/chat-build/deps/srs-v6.0-r0/trunk
output=/mnt/d/chat/_server/Output/linux-x64/Release
test -f "$source/configure"
cd "$source"
if [[ ! -f objs/Makefile ]]; then
    ./configure --rtc=off --srt=off --https=off --h265=off --apm=off --sanitizer=off --backtrace=off --jobs=2
fi
timeout 600 make -j2
mkdir -p "$output"
cp objs/srs "$output/srs"
file "$output/srs"
sha256sum "$output/srs"
