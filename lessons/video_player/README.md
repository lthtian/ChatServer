# Qt 音视频播放器教学工程

第五阶段入口为 `build/media_player.exe`。第三阶段入口 `build/silent_player.exe` 保留，二者共用 VideoView 画布。

第五阶段功能、模块、运行和构建说明见本目录 `MEDIA_PLAYER.md`；知识笔记位于 `docs/音视频学习/05-音画同步与进度跳转.md`。下面保留第三阶段入口的说明。

## 第三阶段：Qt 无声视频播放器

独立的第三阶段教学项目，使用 Qt 5 Widgets、FFmpeg、C++17。打开本地视频后自动播放，支持暂停/恢复、结束后重新播放、播放过程中切换文件，以及窗口缩放时保持画面比例。音频流不解码，因此有音轨的视频也以无声方式播放。

## 运行

```powershell
& 'D:\chat_server\ChatServer\lessons\video_player\build\silent_player.exe' 'D:\test_video.mp4'
```

也可以双击 `build/silent_player.exe`，再点“打开视频”。运行依赖 DLL 和 `platforms` 目录需要保留在 EXE 旁。Qt Creator 可打开本目录 CMakeLists.txt，使用 Qt 5.15.2 / MinGW 8.1.0 x64。

## 代码位置

| 文件 | 内容 |
|---|---|
| `main.cpp` | 创建 QApplication 和窗口，进入事件循环 |
| `video_decoder.h/.cpp` | 后台读包、逐帧解码、转换为独立 QImage，发出时间信息 |
| `player_window.h/.cpp` | VideoView 绘制、PTS 调度、暂停时钟、按钮和解码线程生命周期 |
| `tests/playback_test.cpp` | 使用真实媒体、事件循环和窗口控件的 Qt Test 验收 |
| `tests/run_tests.py` | 生成媒体样本、参考图片，运行并检查测试日志 |

本项目独立保存第三阶段代码，第一、二阶段的 `lessons/media_info` 保留原有示例。解码 API 沿用相同原理；这里每次请求一帧，避免把整个视频提前解码进内存。

第三阶段知识笔记位于 `docs/音视频学习/03-Qt无声视频播放器.md`，重点是播放时钟、图像内存所有权、重绘和线程。构建、运行、验收内容集中放在本 README。

## 范围

面向本地文件、CPU 软件解码和普通 SDR 方形像素视频，选择第一条视频流。使用 QPainter 保持宽高比，未实现音频、进度跳转、网络输入、硬件加速、旋转元数据或 HDR 色调映射。迟到帧立即显示，不实现主动丢帧；解码性能不足时可能卡顿。

时间戳正常时按 PTS 播放，首帧归零。时间戳或帧时长缺失时使用估计值，具体规则写在知识笔记中。视频末尾会继续显示最后一帧至其时长结束。

## 构建

本机配置：Windows，Qt 5.15.2，MinGW 8.1.0 x64，CMake 3.30.5，Ninja；FFmpeg 8.1.1 SDK 位于 `D:\chat\_deps\ffmpeg-8.1.1-full_build-shared`。应用使用 Debug 配置；本机 Qt SDK 提供 Release 库及插件，CMake 按 Qt 导入目标选择实际路径。

首次配置（PowerShell，工作目录为 CMake 所在目录）：

```powershell
Set-Location 'F:\Qt\Tools\CMake_64\bin'
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe -S "D:\chat_server\ChatServer\lessons\video_player" -B "D:\chat_server\ChatServer\lessons\video_player\build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=F:/Qt/Tools/mingw810_64/bin/g++.exe -DCMAKE_MAKE_PROGRAM=F:/Qt/Tools/Ninja/ninja.exe -DCMAKE_PREFIX_PATH=F:/Qt/5.15.2/mingw81_64 -DFFMPEG_ROOT=D:/chat/_deps/ffmpeg-8.1.1-full_build-shared'
```

只编译播放器：

```powershell
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe --build "D:\chat_server\ChatServer\lessons\video_player\build" --config Debug --target silent_player --parallel'
```

本次构建播放器和验收程序的精确命令：

```powershell
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe --build "D:\chat_server\ChatServer\lessons\video_player\build" --config Debug --target silent_player playback_test --parallel'
```

源码改变后使用增量构建；只有 CMake 输入、工具链或依赖配置改变才需要重新配置。这里是 Ninja 构建，不使用 MSBuild 参数。

## 验证记录

2026-10-01：先写真实媒体验收，确认实现不存在时失败，再实现。最终 `silent_player`、`playback_test` / Debug / x64 为 **Full build passed**，成功构建命令耗时 19.80 秒，无编译警告。

| 产物 | 大小 | 修改时间（本地） | 架构 |
|---|---|---|---|
| `build/silent_player.exe` | 3100207 字节 | 2026-10-01 19:20:58 | PE x64 |
| `build/playback_test.exe` | 3670012 字节 | 2026-10-01 19:21:16 | PE x64 |

最初部署步骤因手写 Qt Debug 插件后缀失败：源码编译和链接已完成，但整条构建命令失败。根因是本机 Qt SDK 只提供 Release 插件；已改为使用 Qt 自身的插件导入目标，最终完整构建通过，没有安装或更换工具链。

6 项功能验收全部通过（Qt Test 另计初始化、清理，共 8 passed），约 10.1 秒：

- 样本 40 帧按 0.125 秒间隔交给画布，播放总时长约 5 秒；首帧像素与 FFmpeg 参考结果一致，后续解码未破坏首帧图像。
- 点击真实暂停/播放按钮，暂停期间帧数和位置不变；事件循环仍有响应，恢复后时间连续。
- 变帧率样本按 0、0.25、0.875 秒调度，没有使用固定帧间隔代替 PTS。
- 快速切换文件、音频轨在前时选择视频轨并无声播放、首 PTS 为 5 秒时从零播放、中文文件名、重新播放、居中留黑边及实际绘制颜色。
- 缺失文件、纯音频文件明确报错，并能重新打开有效视频。
- 打开过程中关闭窗口，解码线程正常结束。

另使用真实 Windows 平台插件验证暂停、恢复和重绘，通过。软件测试检查交给画布的时刻和绘制结果，不测量显示器扫描或垂直同步的物理时刻。

完整日志：`build/acceptance.log`；Windows 检查：`build/windows-smoke.log`。预览：`build/player-preview.png`、`build/player-windows.png`。

完整验收命令：

```powershell
python -X utf8 'D:\chat_server\ChatServer\lessons\video_player\tests\run_tests.py' --test 'D:\chat_server\ChatServer\lessons\video_player\build\playback_test.exe' --sample 'D:\test_video.mp4' --screenshot 'D:\chat_server\ChatServer\lessons\video_player\build\player-preview.png'
```

脚本使用已安装的 ffmpeg，临时生成样本，用 Qt offscreen 平台运行真实控件及绘制；Windows 字体目录供离屏绘制使用。测试日志显式写入文件后读取，断言通过数量并拒绝意外警告或错误，不依赖进程返回 0 就判定通过。原始视频不修改。

Windows 平台的单项检查在上述命令末尾增加 `--platform windows --case PauseResumeAndRepaint`，截图可改为 `build/player-windows.png`；此检查会短暂显示窗口。
