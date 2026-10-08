# 媒体处理与 HLS 实验

同一 C++ 工具演示转封装与转码。FFmpeg API 完成全部媒体处理；Qt 5 Core 只提供文件路径、排他创建及重命名，不使用 Qt Multimedia 或 GUI。

## 运行

在本目录的 PowerShell 中执行，目标文件必须尚不存在：

```powershell
.\build\media_process.exe remux ..\video_player\samples\sintel_trailer.mp4 .\build\sintel_faststart.mp4
.\build\media_process.exe remux ..\video_player\samples\sintel_trailer.mp4 .\build\sintel.mkv
.\build\media_process.exe transcode ..\video_player\samples\sintel_trailer.mp4 .\build\sintel
```

`transcode` 串行生成 `480p.mp4`、`360p.mp4`，不覆盖原件。已有实验产物在 `build/results`，可直接用第五阶段 `lessons/video_player/build/media_player.exe` 或聊天客户端的本地播放器打开。

输入可用无音轨的 `D:/test_video.mp4` 和有音轨的 Sintel。转码选第一条普通视频与第一条音轨，输入高度至少 480，输出 H.264 YUV420P；有声音则转为 AAC、48 kHz、双声道。旋转校正、HDR、去隔行、动态格式变化及音频时间线修复不属于此工具范围。转封装仅保留普通音视频轨道，目标容器须支持原编码。

## 阅读顺序

1. `media_io.h`：输入/输出资源分别由谁拥有。
2. `remux.cpp::Remux`：先看最短的“读包 → 换算时间基 → 写包”。
3. `media_io.cpp::Output`：创建轨道、写头、交错写包、写尾及 faststart。
4. `transcoder.h` → `transcoder.cpp::TranscodeFile`：两轨分发及每轨编解码对象。
5. `ConfigureVideo` → `ConvertVideo` → `Encode` / `ReceivePackets`：视频转码。
6. `ConvertAudio` → `Resample` → `EncodeAudio` → `Finish`：PCM FIFO 和各层排空。

7. `hls_package.cpp::PackageHls` / `Segment`：按实际高度筛选档位、串行编码、HLS 封装、生成清单。

知识文档：`docs/音视频学习/08-媒体处理与HLS点播.md`，8.1 讲编解码与转封装，8.2 讲列表、TS/fMP4、HTTP、服务端鉴权与 Qt 切档。

## HLS 生成与发布

在此目录运行（目标目录和同名 `.part` 必须尚不存在）：

```powershell
.\build\media_process.exe hls ..\video_player\samples\sintel_trailer.mp4 .\build\my-hls ts
.\build\media_process.exe hls D:\test_video.mp4 .\build\my-fmp4 fmp4
```

`ts` 是默认值。Sintel 为 854×480，生成 480p/360p；无声样本为 1820×1030，生成 720p/480p/360p。低于 360 的源生成一档偶数高度。每档包含 `index.m3u8` 和约两秒一片的媒体；fMP4 还包含 `init.mp4`。根目录有 `master.m3u8` 与业务产物清单 `catalog.json`。工具不访问云服务器。

聊天中的 MP4 由服务端后台自动运行此工具，无需客户端预先转码。以下命令仅为独立的受控手工发布入口，不要与同一媒体的后台处理任务同时执行：

```powershell
py -3.13 .\publish_hls.py <media_id> .\build\my-hls
```

脚本依赖 Paramiko，本机已安装在 `build/python`；迁移环境可先 `py -3.13 -m pip install --target .\build\python paramiko`。运行时提示输入 SSH 密码；主机公钥必须已存在于当前用户的 SSH known_hosts 中，脚本不会自动信任未知主机。

`media_id` 来自上传/发布结果或聊天消息的 `media.media_id`，不是用户 ID 或历史消息 ID。脚本核对云端原件摘要后才发布，不接受把另一视频的 HLS 绑定上去。

服务端对象布局是 `objects/<media_id>/hls/<revision>/...`。外部资源名中的点映射为对象键中的下划线，例如 `480p/seg_00000.ts` 保存为 `480p/seg_00000_ts`；这是存储实现，播放器仍使用标准 `.ts` URL。`catalog` 和 `status` 位于该视频的 hls 目录，服务端只公开清单白名单中的资源。

先完整上传并核验分片，再原子替换 `catalog`，重复处理时先前版本仍可读。已有同版本目录不会被覆盖。失败时先查看 `status`、本机异常和 SSH 连接；确认没有发布进程运行后再人工处理 `publish_lock` 或残留版本。脚本不自动删除资源；退出/断电的恢复和旧版本清理尚未实现。

正常链路是上传原件 → MediaJob 入队 → Linux 子进程执行 `media_process hls` → 校验并发布完整 HLS。处理中仍可在线播原件，完成后重新点开视频取得实际清晰度列表；下载按钮始终下载原始 MP4。手工脚本的 status 文件与数据库中的自动任务状态是两套入口，自动任务以 MediaJob 为准。

当前构建、部署及真实上传验证见 `docs/STAGE8_AUTOMATIC_VOD_DELIVERY.md`；首次独立 HLS 实验记录在 `docs/STAGE8_DELIVERY.md`。

## Linux 服务端运行程序

本机 WSL 编译，云服务器只运行。`deploy/build-media-server.sh` 构建 ChatServer 和 media_process；已有构建目录分别是 `/opt/chat-build/build/server-Release` 和 `/opt/chat-build/build/media-process-Release`，产物复制到 `D:/chat/_server/Output/linux-x64/Release`。

Linux 依赖 Qt5 Core 和 FFmpeg 的 avformat、avcodec、avutil、swscale、swresample 开发库；CMake 通过 Qt5 配置和 pkg-config 查找。当前 WSL 的 Qt 开发包解压在 `/opt/chat-build/deps/qt/usr`，未更改 Windows Qt SDK。云端已有匹配的 Qt Core 5.15.13、FFmpeg 6.1 运行库，无需 GUI。

服务端通过绝对路径 `CHAT_MEDIA_PROCESS` 启动该程序；`CHAT_HLS_SEGMENT_TYPE=ts` 或 `fmp4` 控制分片容器。原件参数、任务输出目录由服务端生成，不接受客户端指定可执行程序或命令。

观察自适应选择（使用真实分片字节数，带宽变化是受控模拟，不修改系统网络）：

```powershell
py -3.13 .\abr_observe.py .\build\hls-sintel
```

输出吞吐估计、所选档、下载时间和剩余缓冲。初始带宽 2.5 Mbit/s，中段降到 0.5，之后升到 3；采用 70% 余量、EWMA 平滑和连续三次满足才升档。它不是 Qt 播放器内置 ABR，也没有模拟 RTT、重试或音频设备。

## 本机构建

复用已有 Qt 5.15.2、MinGW 8.1 x64、FFmpeg 8.1.1 SDK。生成器为 Ninja，配置 Debug，不使用 Visual Studio/MSBuild。构建工作目录为 `F:/Qt/Tools/CMake_64/bin`。

首次配置（CMakeLists 或依赖路径变化时才需要重新配置）：

```bat
cmd.exe /d /c "set PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%&& cmake.exe -S D:\chat_server\ChatServer\lessons\media_process -B D:\chat_server\ChatServer\lessons\media_process\build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=F:/Qt/Tools/mingw810_64/bin/g++.exe -DCMAKE_MAKE_PROGRAM=F:/Qt/Tools/Ninja/ninja.exe -DCMAKE_PREFIX_PATH=F:/Qt/5.15.2/mingw81_64 -DFFMPEG_ROOT=D:/chat/_deps/ffmpeg-8.1.1-full_build-shared"
```

增量构建；下面是本轮实际构建命令的 PowerShell 写法：

```powershell
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe --build "D:\chat_server\ChatServer\lessons\media_process\build" --config Debug --target media_process --parallel > "D:\chat_server\ChatServer\lessons\media_process\build\build.log" 2>&1'
```

输出目录会同时部署 FFmpeg、Qt Core 和 MinGW 运行库。其他机器需提供匹配架构/ABI 的 SDK，再按当地路径配置。

## 必要验证记录

2026-10-03：目标 `media_process`，Debug、PE x64，**Full build passed**，最终完整构建耗时约 3.50 秒，编译日志无警告。产物 `build/media_process.exe`，2,063,347 字节，修改时间 17:24:04（北京时间）。SHA-256：`cfefd1cbd99cfb278eb86454bbc58dbd6067dda7963b0000632e90dd5e6e4a17`。

首次编译遇到 MinGW 8.1 标准库 `std::filesystem` 头文件编译错误；确认编译进程已退出后，文件操作采用与音频实验相同的 Qt Core API，并按依赖变更重新配置，以上为修正后的完整构建结果。

| Sintel 产物 | 尺寸 | 视频平均码率 | 文件字节数 | 处理耗时 |
|---|---|---|---|---|
| 原件 | 854×480 | 537,875 bit/s | 4,372,373 | — |
| faststart 转封装 | 854×480 | 537,875 bit/s | 4,369,173 | 0.025 s |
| 480p 转码 | 854×480 | 449,701 bit/s | 3,806,885 | 3.349 s |
| 360p 转码 | 640×360 | 256,644 bit/s | 2,336,460 | 2.463 s |

耗时只是本机单次观察，包含读写，不是性能基准。CRF/VBV 不保证平均码率等于设置的最大码率。

验证使用真实工具产物与 FFmpeg/ffprobe：

- 9 个输出（MP4/MKV、两档有声/无声、重采样片段）均完整解码成功，并在 2.1 秒位置 seek 后解码；FFmpeg 使用 `-v error -xerror`，无错误输出。
- Sintel 两档均保留 1,253 幅视频画面；无声样本两档均为 40 帧、5 秒，未凭空添加音轨。
- 两份 MP4 转封装的各轨道包摘要、PTS、DTS、duration 与输入一致；允许不同轨道间重新交错。首次验证按全文件包列表比较，发现同 DTS 的音视频包交错位置变化，改为按各轨道内部序列核对。
- 所有 MP4 的 moov 位于 mdat 之前；转码后各轨道 DTS 单调递增。
- 从真实 Sintel 派生 44.1 kHz 单声道短片验证重采样，输出 48 kHz 双声道；保留约 0.5 秒音轨起始偏移，并完成非整 1024 采样的尾部处理。
- 输出已存在、输入不存在两种错误均捕获并核对信息；原始两个样本 SHA-256 未变，未遗留 `.part`。

详细命令和 ffprobe 数据在 `build/verification.json`，本次验证脚本在 `build/verify.py`。这是构建目录内的一次必要验证，不引入测试框架。验证确认可解码和定位，未声称已经人工试听全部输出。
