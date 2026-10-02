# 第五阶段：本地音视频播放器

在 video_player 工程中使用 Qt 5.15.2 Widgets/Multimedia、FFmpeg 8.1.1 和 C++17。选择第一条视频与音轨，支持有声播放、无音轨播放、暂停恢复、停止重播、点击或拖动进度条、音量静音和全屏。

## 运行

```powershell
& 'D:\chat_server\ChatServer\lessons\video_player\build\media_player.exe' 'D:\chat_server\ChatServer\lessons\video_player\samples\sintel_trailer.mp4'
```

也可以双击 EXE 后点“打开视频”。EXE 旁的 FFmpeg/Qt/MinGW DLL、`platforms` 和 `audio` 插件目录需要保留。音频输出使用系统默认设备；无设备时显示明确错误，有音轨文件不会悄悄变成无声播放。

| 操作 | 行为 |
|---|---|
| 空格 / 播放按钮 | 暂停、恢复；结束或停止后从头播放 |
| 点击进度条 | 按时间比例定位到点击位置 |
| 拖动进度条 | 显示目标时间，松手执行一次定位，保留原播放意图 |
| 左右键 | 前后跳五秒 |
| 音量 / 静音 | 调整本播放器输出，静音保留原音量值 |
| F / 全屏按钮 | 切换全屏 |
| Esc | 退出全屏 |

暂停时 seek 会显示目标预览并保持暂停。状态文字的悬浮提示显示选用的音频设备与格式。

## 模块和阅读入口

| 文件 | 职责与关键函数 |
|---|---|
| `media_main.cpp` | QApplication、窗口与事件循环入口 |
| `media_decoder.h/.cpp` | FFmpeg 工作线程；Open、Read、Send/Receive、Seek、ConvertAudio |
| `audio_output.h/.cpp` | Qt 音频设备；Open、Restart、Pump、PositionUs、Pause/Resume |
| `media_window.h/.cpp` | GUI、公共时钟与播放状态；ConnectDecoder、TryStart、Tick、StartSeek |
| `video_view.h/.cpp` | 两个教学入口共用的 QImage 等比例绘制 |
| `tests/media_smoke.cpp` | 真实样本解码、进度操作和实际设备输出的必要验证 |

第五阶段头文件中的函数、信号和成员均附有中文说明；源文件在声画共用起点、设备计数、跨线程数据所有权和定位清理等位置解释原因。知识文档集中讲 Qt 音频 API 和概念关系，运行记录留在本文件。

## 构建

沿用已配置的 Ninja / Debug / MinGW 8.1.0 x64。CMake 缓存使用：

- CMake：`F:/Qt/Tools/CMake_64/bin/cmake.exe`。
- Qt：`F:/Qt/5.15.2/mingw81_64`。
- FFmpeg：`D:/chat/_deps/ffmpeg-8.1.1-full_build-shared`。

由于增加 Multimedia 组件和目标，本轮先在 CMake 所在目录重新配置现有构建目录：

```powershell
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe -S "D:\chat_server\ChatServer\lessons\video_player" -B "D:\chat_server\ChatServer\lessons\video_player\build"'
```

只构建播放器：

```powershell
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe --build "D:\chat_server\ChatServer\lessons\video_player\build" --config Debug --target media_player --parallel'
```

本轮播放器与必要验证目标的准确构建命令，工作目录均为 `F:/Qt/Tools/CMake_64/bin`：

```powershell
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe --build "D:\chat_server\ChatServer\lessons\video_player\build" --config Debug --target media_player media_smoke --parallel'
```

构建不自动运行程序或测试。显式运行验证时，在 build 目录执行：

```powershell
.\media_smoke.exe -o media-smoke.log,txt
```

## 必要验证记录

2026-10-02，`media_player`、`media_smoke` / Debug / x64：**Full build passed**。首次 Ninja 构建日志终点为 26.218 秒。播放器最终源码构建耗时 8.64 秒；随后更新验证代码的最终增量构建耗时 5.44 秒，播放器保持最新，验证程序更新。无编译警告。

| 产物 | 字节数 | 修改时间（本地） | PE 架构 |
|---|---:|---|---|
| `build/media_player.exe` | 5250205 | 2026-10-02 16:26:58 | x64 / 0x8664 |
| `build/media_smoke.exe` | 6090456 | 2026-10-02 16:32:16 | x64 / 0x8664 |

使用现成 Sintel 预告片及原始无音轨文件，3 项功能检查通过（包含 Qt Test 初始化与清理，共 **5 passed、0 failed、0 skipped**），测试内部耗时 13.411 秒，命令耗时 13.78 秒：

- 解码得到真实 RGB 图像和双声道 PCM；对 0、13.765432、50.123456 秒定位，核对目标画面及音频采样位置，并排空尾部。
- 无音轨文件播放、暂停冻结、暂停定位、点击真实滑槽、结束、重播与停止。
- 接入实际默认声音设备，验证播放位置推进、暂停恢复、暂停时定位到十秒、点击滑槽跳到尾部、尾音结束与剩余视频显示、重播、停止及切换无音轨文件。

初次设备检查失败的原因是系统实际枚举到零个音频输出设备；连接耳机后枚举到两个，全部验证通过。Qt 非空默认设备描述不保证存在真实设备，程序为此检查 availableDevices。

另一项检查最初错误地要求音频首帧必须在零点，ffprobe 对照确认该样本首个解码音频帧为 0.021333 秒；按实际媒体时间戳修正检查，保留播放器的起始静音与公共时间轴。

日志位于 `build/media-smoke.log`，实际窗口截图为 `build/media-preview.png`。验证检查真实设备消费和软件呈现状态；没有通过回录或外部仪器测量耳机物理出声与屏幕扫描的偏差，也没有把短片段检查称为整部影片长时间同步验证。

## 本例边界

面向本地、连续时间戳、普通 SDR 软件解码；不包含网络、字幕、倍速、硬件解码和动态采样格式变化。音频时钟的缓冲扣减按已核对的 Qt 5.15.2 Windows 后端实现，换其他后端需重新核对。

应用提前准备约 150ms 数据，图像缓存硬上限为 64 MiB/180 张，PCM 积压上限为两秒；输入交错异常时明确报错，不无限增长内存。后续队列专题再研究容量配置、分轨包缓存和更完整的背压策略。
