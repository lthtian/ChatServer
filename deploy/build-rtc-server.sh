#!/usr/bin/env bash
# 将指定 worktree 同步到独立 WSL 构建目录，避免编译到 main 的源码。
set -euo pipefail
source=$(realpath "${1:?pass the WSL path of the server worktree}")
workspace=/opt/chat-build/rtc
output=/mnt/d/chat/_server/Output/linux-x64/rtc
test -f "$source/src/server/live_rtc.cpp"
test -f "$source/migrations/006_live_transport.sql"
mkdir -p "$workspace/source"
exec 9>"$workspace/.build.lock"
flock -n 9 || { echo 'RTC build workspace is busy' >&2; exit 1; }
if [[ -f "$workspace/.source-origin" ]]; then
    test "$(cat "$workspace/.source-origin")" = "$source"
else
    printf '%s\n' "$source" > "$workspace/.source-origin"
fi
test "$(realpath "$workspace/source")" = /opt/chat-build/rtc/source
rsync -a --delete --exclude='.git' --exclude='build/' --exclude='bin/' \
    --exclude='docs/' --exclude='openspec/' "$source/" "$workspace/source/"
if [[ ! -s "$workspace/build/CMakeCache.txt" ]]; then
    /usr/bin/cmake -S "$workspace/source" -B "$workspace/build" -DCMAKE_BUILD_TYPE=Release
fi
/usr/bin/time -v /usr/bin/cmake --build "$workspace/build" --target ChatServer --parallel 2
mkdir -p "$output"
cp "$workspace/source/bin/ChatServer" "$output/ChatServer"
file "$output/ChatServer"
sha256sum "$output/ChatServer"
