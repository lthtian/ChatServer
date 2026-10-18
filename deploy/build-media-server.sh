#!/usr/bin/env bash
# 在本机 WSL 编译两个 Linux 程序，不在云服务器编译。
set -euo pipefail
workspace=/opt/chat-build
source=/mnt/d/chat_server/ChatServer
output=/mnt/d/chat/_server/Output/linux-x64/Release
qt=${CHAT_BUILD_QT_PREFIX:-/opt/chat-build/deps/qt/usr}
exec 9>"$workspace/.build.lock"
flock -n 9 || { echo 'Build workspace is busy' >&2; exit 1; }
test "$(cat "$workspace/.source-origin")" = "$source"
test "$(realpath "$workspace/source")" = /opt/chat-build/source
rsync -a --delete --exclude='.git/' --exclude='build/' --exclude='bin/' \
  --exclude='openspec/' "$source/" "$workspace/source/"

if [[ ! -f "$workspace/build/server-Release/CMakeCache.txt" ]]; then
  /usr/bin/cmake -S "$workspace/source" -B "$workspace/build/server-Release" -DCMAKE_BUILD_TYPE=Release
fi
/usr/bin/time -v /usr/bin/cmake --build "$workspace/build/server-Release" --target ChatServer --parallel 2
mkdir -p "$output"
cp "$workspace/source/bin/ChatServer" "$output/ChatServer"

if [[ ! -f "$workspace/build/media-process-Release/CMakeCache.txt" ]]; then
  /usr/bin/cmake -S "$source/lessons/media_process" -B "$workspace/build/media-process-Release" \
    -DCMAKE_BUILD_TYPE=Release -DQt5_DIR="$qt/lib/x86_64-linux-gnu/cmake/Qt5" \
    -DQt5Core_DIR="$qt/lib/x86_64-linux-gnu/cmake/Qt5Core"
fi
/usr/bin/time -v /usr/bin/cmake --build "$workspace/build/media-process-Release" --target media_process --parallel 2
cp "$workspace/build/media-process-Release/media_process" "$output/media_process"
file "$output/ChatServer" "$output/media_process"
sha256sum "$output/ChatServer" "$output/media_process"
