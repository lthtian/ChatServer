# 9.1 文件模拟直播：最小 C++ RTMP 推流

一个 `main.cpp`：本地 MP4 → 按 DTS 控制发送节奏 → FLV 封装 / RTMP 发布。要求 H.264 视频和可选 AAC 音频。只推送一次，文件结束就退出；不转码、不自动循环或重连、不修改聊天项目。

知识文档：`docs/音视频学习/09-直播推拉流与分发.md`。核心入口 `Push()`，先读它的循环，再看输入输出初始化；资源和超时辅助代码在文件前部。

## 9.2：推到聊天直播

Qt 客户端选择会话，点“创建直播”，复制弹窗中的完整 RTMP 地址，然后用本节 EXE 替换下面的第二个参数：

```powershell
& 'D:/chat_server/ChatServer/lessons/live_push/build/live_push.exe' 'D:/chat_server/ChatServer/lessons/video_player/samples/sintel_trailer.mp4' '<从聊天客户端复制的完整RTMP地址>'
```

保持聊天登录，在票据签发后五分钟内建立连接。聊天另一端收到卡片后，可选择 HTTP-FLV 或 HLS 观看。Sintel 一次约52秒，播放结束自然断流；再次运行可重新推送同一场未结束直播。过期先刷新推流信息；主动结束后需创建另一场。公网推流需要阿里云安全组放行 TCP1935，观看走现有80端口。

Linux 版也可在本机 WSL 通过 `deploy/build-live-push.sh` 编译，产物 `D:/chat/_server/Output/linux-x64/Release/live_push`，不是 Windows EXE。该工具在云端可用于内部链路诊断；云端只运行，不编译。SRS 与 ChatServer 的配置、部署记录见 `docs/STAGE9_LIVE_DELIVERY.md`。

## Windows：本机收发，不依赖 Linux

产物：`D:/chat_server/ChatServer/lessons/live_push/build/live_push.exe`。运行目录旁已复制所需 DLL。

先在 PowerShell 终端 A 启动单连接 RTMP 接收器：

```powershell
& 'D:/chat/_deps/ffmpeg-8.1.1-full_build-shared/bin/ffmpeg.exe' -hide_banner -loglevel warning -n -listen 1 -timeout 60 -i 'rtmp://127.0.0.1:19350/live/lesson' -map '0:v:0' -map '0:a:0?' -c copy 'D:/chat_server/ChatServer/lessons/live_push/build/received.flv'
```

`-n` 防止覆盖已有文件；再次运行请使用另一个输出文件名。`-listen 1` 是 FFmpeg RTMP 接收模式，`-timeout 60` 给连接建立预留 60 秒。端口若已被其他程序使用，应同时修改两端地址，不结束其他程序。

然后在终端 B 推流：

```powershell
& 'D:/chat_server/ChatServer/lessons/live_push/build/live_push.exe' 'D:/chat_server/ChatServer/lessons/video_player/samples/sintel_trailer.mp4' 'rtmp://127.0.0.1:19350/live/lesson'
```

样本约 52 秒，媒体数据按播放节奏发出。观察 `media_s` 与 `elapsed_s` 接近；建立连接的耗时不计入日志里的 `elapsed_s`。Ctrl+C 请求退出。

接收完成后可以用现成播放器检查文件：

```powershell
& 'D:/chat/_deps/ffmpeg-8.1.1-full_build-shared/bin/ffplay.exe' -autoexit 'D:/chat_server/ChatServer/lessons/live_push/build/received.flv'
```

如要直接边接收边观看，可关闭已经自然结束的接收流程后，在另一次实验中用以下命令**代替终端 A 的 ffmpeg 接收器**，再运行终端 B：

```powershell
& 'D:/chat/_deps/ffmpeg-8.1.1-full_build-shared/bin/ffplay.exe' -autoexit -listen 1 -timeout 60 'rtmp://127.0.0.1:19350/live/lesson'
```

FFmpeg/ffplay 监听模式不是多观众服务器，不能同时占用同一端口。直接观看命令作为操作入口提供，本轮自动验证采用录制再解码，没有做主观出声与画面验收。

## SRS 接入位置（待 Linux 恢复后联调）

推流程序无需改变读包/定速逻辑，只把地址改为已启动 SRS 的 `rtmp://主机:1935/live/lesson`。由 SRS 提供 HTTP-FLV/HLS 播放入口；路径取决于实际配置。此时观众连接 SRS，不直接连接推流程序。

SRS 尚未部署或启动；多观众中途加入、GOP 缓存、HTTP-FLV/HLS 分发与推流重连尚未验证。Qt 直播模式和聊天入口也未在此示例中修改。

## Windows 构建

目标 `live_push`，Debug / Windows x64，MinGW 8.1 + Ninja，FFmpeg SDK 8.1.1。无 Qt 依赖。

在 `F:/Qt/Tools/CMake_64/bin` 工作目录，通过 CMD 配置（仅首次或 CMake 配置变化时）：

```bat
cmd.exe /d /c "set PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%&& cmake.exe -S D:/chat_server/ChatServer/lessons/live_push -B D:/chat_server/ChatServer/lessons/live_push/build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=F:/Qt/Tools/mingw810_64/bin/g++.exe -DCMAKE_MAKE_PROGRAM=F:/Qt/Tools/Ninja/ninja.exe -DFFMPEG_ROOT=D:/chat/_deps/ffmpeg-8.1.1-full_build-shared"
```

实际构建命令（PowerShell 中启动 CMD；工作目录同上）：

```powershell
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe --build D:/chat_server/ChatServer/lessons/live_push/build --config Debug --target live_push --parallel'
```

Linux CMake 分支使用 pkg-config 查找 FFmpeg，尚未构建验证。本阶段不使用虚拟机或远程服务器。

## 本轮验证记录：2026-10-08

- **Full build passed**：上述构建命令成功，用时 6.15 秒，包含因 CMake 编译定义变化而自动再生成；产物 `build/live_push.exe`，466542 字节，PE Machine `0x8664`（x64），修改时间 2026-10-08 23:49:36。首次配置 5.63 秒。
- 首次构建 8.73 秒失败：独立工程未设置 WINVER，MinGW 未声明 `WC_ERR_INVALID_CHARS`，另有 NOMINMAX 重定义警告。检查工具链头文件并确认无遗留编译进程后，设置与已有项目相同的 Windows API 版本、条件定义宏；没有安装或改变工具链。最终构建无警告。
- 本机真实 RTMP：FFmpeg SDK 中的接收端监听回环地址，C++ 程序推送；记录到 FLV 后，比较输入/输出编码包数量并完整解码，未使用 mock。
- Sintel 截取约 6 秒，不重新编码：146 个 H.264 包、282 个 AAC 包全部接收，完整解码通过。推送日志 `media_s=6` 对应 `elapsed_s≈6.00011`；进程总耗时 9.945 秒还包含输入探测、连接建立及收尾。
- 原始 `D:/test_video.mp4`：40 个 H.264 包全部接收，无音轨正常；完整解码通过，进程总耗时 4.970 秒。
- 不存在的输入文件：返回 1，明确报告 `open MP4: No such file or directory`。
- 两次接收端均在发布端正常关闭后报告一条 `Error during demuxing: I/O error`；验证脚本明确检查该预期 EOF 诊断且拒绝其他诊断，接收进程退出码 0。包数量一致且完整解码无警告。此现象仅记录本机 FFmpeg 监听端表现，不泛化为所有网络 I/O 错误都可忽略。
- 证据保存在被忽略的 `build/verification.json`、`build/publisher-*.log`、`build/receiver-*.log`，以及一次性验证脚本 `build/verify_local.py`。临时样本和接收文件也在 build 内，不修改原始样本。
- 未验证：SRS、多观众分发、HTTP-FLV/HLS 直播、自动重连、长时间网络拥塞、Ctrl+C 交互退出与无响应对端超时；这些不在本轮通过结论内。

## 查阅依据

- FFmpeg 官方 remux 示例：https://ffmpeg.org/doxygen/trunk/remux_8c-example.html
- FFmpeg RTMP 协议选项：https://ffmpeg.org/ffmpeg-protocols.html#rtmp
- SRS HTTP-FLV：https://ossrs.io/lts/en-us/docs/v6/doc/flv
- SRS HLS：https://ossrs.io/lts/en-us/docs/v6/doc/hls

参考的是接口职责；本示例的资源管理、定速、超时和错误处理以当前代码为准。
