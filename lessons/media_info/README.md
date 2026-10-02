# 媒体信息查看与视频帧导出

这是第一、二阶段共用的独立控制台示例。查看包模式读取容器、流和前若干个包；解码模式将第一条视频流完整解码，打印帧信息并导出首帧 PPM 图片。不需要 Qt 窗口，也不修改输入文件。

## 代码阅读顺序

教学代码集中在 `main.cpp`。先读 `Inspect()`，其中的中文注释按照实际执行顺序编号：

1. `avformat_open_input()` 打开文件，得到管理输入的 `AVFormatContext`。
2. `avformat_find_stream_info()` 探测流的编码、分辨率等信息；它可能预读数据，预读的包由 FFmpeg 管理，后续正常读包不需要手动回退文件位置。
3. 遍历 `input->streams`，通过 `codecpar` 查看编码参数，通过 `time_base` 查看时间戳单位。
4. 循环调用 `av_read_frame()`，根据 `packet->stream_index` 找到所属流并打印包的信息；解码模式把选中流的包送入 `VideoDecoder::Send()`。随后用 `av_packet_unref()` 释放本次数据引用。
5. 解码模式在输入 EOF 后调用 `Send(nullptr)` 取完缓存帧。离开作用域，`unique_ptr` 的删除器释放包对象、解码实例并关闭输入。异常退出也释放资源。

然后读 `Seconds()` 理解时间换算，读 `CheckResult()` 理解 FFmpeg 负数错误码。入口函数负责检查文件路径和包数量；Windows 的 `Argument()` 负责 UTF-16 到 UTF-8 的路径转换。

第二阶段顺着 `VideoDecoder` 构造函数 → `Send()` → `Receive()` → `SaveFirstFrame()` 阅读。构造函数只执行一次，持续保存这个视频流的解码状态；送包后循环接收所有当前可用的帧。`AVCodec` 是实现描述，`AVCodecContext` 才是逐流独立的工作实例。

## 运行

完成构建后，在 PowerShell 中执行：

```powershell
& 'D:\chat_server\ChatServer\lessons\media_info\build\media_info.exe' 'D:\test_video.mp4' 8
```

第二个参数是最多读取的包数，省略时为 8。改成 100，可观察当前样本读完 40 个包后正常结束。

完整解码并导出第一张图片：

```powershell
& 'D:\chat_server\ChatServer\lessons\media_info\build\media_info.exe' 'D:\test_video.mp4' --decode 'D:\chat_server\ChatServer\lessons\media_info\build\my-first-frame.ppm'
```

输出路径的父目录需要存在，目标文件必须尚不存在；重复运行可以换一个文件名。PPM 文件由 RGB 字节直接组成，不需要图片编码器。这个示例只转换首帧，其余帧仍全部解码并统计。没有视频流时明确报错；存在音轨或多条视频轨时，只解码第一条视频轨，其他包仍打印信息。

输出各行含义：

| 行首 | 内容 |
|---|---|
| `format` | 容器名称、流数量、总时长、平均码率 |
| `stream` | 流编号、类型、编码、时间基，以及视频或音频参数 |
| `audio` | 是否发现音频流，无音轨属于正常结果 |
| `packet` | 实际读取序号、所属流、编码字节数、时间戳、时长、关键包标记 |
| `summary` | 已读取包数、是否实际遇到文件末尾 |
| `decoder` | 本次选择的流编号和解码器名称 |
| `frame` | 实际输出帧序号、呈现时间、尺寸、像素格式、行跨度、I/P/B 类型和输出阶段 |
| `image` | 首帧导出的格式、尺寸及有效 RGB 字节数 |
| `drain` | 文件 EOF 后取出缓存帧的开始与结束 |
| `decode_summary` | 总输出帧数、排空阶段输出帧数 |

`packet index` 是本次程序的读取计数，不是显示帧编号。`pts_s`、`dts_s`、`duration_s` 使用该包所属流的时间基换算；`format duration_s` 则使用 `1/AV_TIME_BASE`。时间戳未知时输出 `unknown`。包的 `duration=0` 表示未提供时长，不能据此认定画面没有显示时间。

`key=yes` 来自 `AV_PKT_FLAG_KEY`。它不是完整的 I/P/B 类型分析；解码模式的 `frame picture` 来自 `AVFrame::pict_type`。`eof=no` 表示达到读取数量上限时尚未遇到 EOF，不保证后面一定还有包。

`frame time_s` 使用输出帧的 `best_effort_timestamp` 和所选流时间基换算；`phase=packets` 表示常规送包期间输出，`phase=drain` 表示提交结束信号后输出。包的 PTS 不能直接当作当前输出帧的时间。

## 学习时的对照点

对当前样本，先观察只有一条视频流且 `audio present=no`。接着观察前几个包的 DTS 递增、PTS 跳动，以及 `pts=8192` 对应 `pts_s=0.500000`。这些信息直接来自读包结果，没有手动排序。

解码模式下，包 0 和包 1 送入后暂时没有输出，包 2（PTS 0.25 秒）送入后才收到帧 0（PTS 0 秒）。所有 40 个包读完后仍有两帧留在解码器里；提交空指针后输出 PTS 4.75 秒、4.875 秒的两帧。最终日志为 `decode_summary frames=40 drained_frames=2`。这种延迟不意味着一幅图一定被物理切成多个包。

首帧为 1820 × 1030、yuv444p，三个平面的行跨度均为 1856 字节。转换后 RGB24 每行只写入 `1820 × 3` 个有效字节，不能把内存填充写入图片。`SaveFirstFrame()` 使用 FFmpeg 8.1.1 的 `sws_alloc_context()` 和 `sws_scale_frame()`，依据帧颜色信息进行转换，并用 `sws_free_context()` 释放转换上下文。

实测输出位于 `build/first-frame.ppm`；`build/decode-sample.txt` 为完整运行日志。`build/first-frame.png` 是供预览的 PNG 副本，由 FFmpeg 命令行从 PPM 转换，教学程序自身只写 PPM。本阶段尚未实现窗口显示、按时间播放或音频解码。

## 本机构建

独立 CMake 工程，目标 `media_info`；Windows 10 及以上 API，Debug，x64，MinGW 8.1.0，CMake 3.30.5，Ninja。使用 Qt 5 安装附带的编译器，不链接 Qt 库。

FFmpeg 8.1.1 开发包位于 `D:\chat\_deps\ffmpeg-8.1.1-full_build-shared`，包含 `include`、`lib`、`bin`。压缩包 SHA256 已与发布者记录核对：`7a6c3e86f54b0dd3bceff31f81bf3b70397171fcf35d1a13b9ed9cbf1040fc18`。依赖保留在工程外，构建后将运行所需 DLL 复制到 EXE 所在目录。

已有 `build` 可直接增量编译。以下命令在 PowerShell 中执行，工作目录先进入 CMake 程序所在位置；PATH 只在这次 CMD 子进程中调整：

```powershell
Set-Location 'F:\Qt\Tools\CMake_64\bin'
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe --build "D:\chat_server\ChatServer\lessons\media_info\build" --config Debug --target media_info --parallel'
```

仅在首次配置或修改 CMake 输入、编译器、依赖位置时执行配置命令：

```powershell
Set-Location 'F:\Qt\Tools\CMake_64\bin'
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe -S "D:\chat_server\ChatServer\lessons\media_info" -B "D:\chat_server\ChatServer\lessons\media_info\build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=F:/Qt/Tools/mingw810_64/bin/g++.exe -DCMAKE_MAKE_PROGRAM=F:/Qt/Tools/Ninja/ninja.exe -DFFMPEG_ROOT=D:/chat/_deps/ffmpeg-8.1.1-full_build-shared'
```

Qt Creator 可打开本目录 `CMakeLists.txt` 阅读或调试，使用相同 MinGW 工具链和 `FFMPEG_ROOT`。本次验证使用上述命令行构建，未验证 Qt Creator 中的运行配置。

## 验证记录

2026-10-01：`media_info / Debug / x64` 构建结果为 **Full build passed**，增量构建耗时 7.97 秒，无编译警告。产物 `D:\chat_server\ChatServer\lessons\media_info\build\media_info.exe` 已更新，651187 字节，修改时间 2026-10-01 16:22:07（本地时间），PE 架构 `pei-x86-64`。构建命令即上一节的增量编译命令。本次因 CMake 链接依赖增加 `swscale`，先按所列配置命令更新现有 Ninja 构建目录。

两个阶段均先编写真实文件验收并确认功能尚未实现时失败，再实现功能。最终 10 项读包测试和 11 项解码测试全部通过。读包字段与顺序逐项对照 ffprobe；解码帧时间、类型、尺寸、像素格式也与 ffprobe 对照，首帧 RGB 字节与 FFmpeg 命令行输出完全一致。

覆盖中文输入输出路径、数量限制、EOF 排空、第一帧仅在排空时输出、音频在前的视频流选择、多视频流选择、yuv420p、BT.709 完整范围转换，以及参数错误、缺失文件、无效媒体、没有视频流、输出目录缺失和已有文件保护。完整范围 H.264 解码为 YUVJ444P 时，swscale 会输出一条弃用格式提示；该测试精确检查此提示及 RGB 结果，没有关闭库日志。其他预期错误同样由测试捕获并检查。生成样本位于临时目录，测试结束后清理。

运行验收脚本（已安装的 `ffmpeg`、`ffprobe`、`python` 在 PATH 中）：

```powershell
python -X utf8 'D:\chat_server\ChatServer\lessons\media_info\tests\media_info_test.py' --tool 'D:\chat_server\ChatServer\lessons\media_info\build\media_info.exe' --sample 'D:\test_video.mp4' --ffmpeg ffmpeg --ffprobe ffprobe
python -X utf8 'D:\chat_server\ChatServer\lessons\media_info\tests\video_decode_test.py' --tool 'D:\chat_server\ChatServer\lessons\media_info\build\media_info.exe' --sample 'D:\test_video.mp4' --ffmpeg ffmpeg --ffprobe ffprobe
```

测试使用 FFmpeg/ffprobe 8.1.1，部分断言针对当前样本的已知时间戳；更换样本时需要同步调整这些样本断言。
