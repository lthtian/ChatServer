# 音频解码与 PCM 导出

第四阶段独立控制台示例。读取输入文件的第一条音轨，逐包解码、打印音频帧信息，通过 libswresample 转成固定的 48000 Hz、stereo、S16 小端交错 PCM，再写出裸采样文件。

教学实现集中在 `main.cpp`，核心由 FFmpeg 完成；Qt 5 Core 只负责命令行与 Unicode 文件路径，不需要窗口或音频设备。本例不直接发声，也不按实际播放时间等待。

知识笔记：`D:\chat_server\ChatServer\docs\音视频学习\04-音频解码与PCM.md`。

## 样本与输出

| 文件 | 用途 |
|---|---|
| `samples/test_video_with_audio.mp4` | 原视频的带音轨副本，视频流复制，新增 44.1 kHz 双声道 AAC |
| `build/decoded.pcm` | 示例实际导出的 48 kHz、双声道、S16 小端交错采样 |
| `build/decoded.wav` | 将上述 PCM 封装为 WAV 后的试听文件 |
| `build/decode-sample.txt` | 完整帧信息、转换数量和结束统计 |

测试音左声道为 440 Hz，右声道为 880 Hz，浮点波形幅度均为 0.1。MP4 的视频仍为原 1820×1030、8 fps、40 帧 H.264。原文件 `D:\test_video.mp4` 保持无音轨，作为“没有音频”的对照。

生成副本的命令如下，在示例目录运行。`-n` 防止覆盖已有输出：

```powershell
ffmpeg -v error -nostdin -n -i 'D:\test_video.mp4' -f lavfi -i 'aevalsrc=0.1*sin(2*PI*440*t)|0.1*sin(2*PI*880*t):s=44100:d=5:c=stereo' -map 0:v:0 -map 1:a:0 -c:v copy -c:a aac -b:a 128k -t 5 -movflags +faststart 'samples/test_video_with_audio.mp4'
```

原文件 SHA256：`BBCC49A308156A62F0C93DFFE16D72EF3C662AFF791E75EA5B1E60C1B59AF116`。

带音轨副本 SHA256：`0223183522FFF11D6F2C567C6598CEC490C22612A02A7C8F2CB7E280C4B94032`。

## 阅读代码

1. `Decode()`：打开容器、探测轨道、选第一条音轨，循环读包并筛选 stream_index。
2. `AudioDecoder` 构造函数：建立音频解码器、排他创建输出文件。
3. `Send()` / `Receive()`：提交编码包、取得音频 AVFrame，处理 EAGAIN 和 EOF。
4. `PrepareResampler()`：使用首帧的真实采样率、样本格式、声道布局建立转换器。
5. `Convert()`：计算输出容量、转换采样、仅写出实际生成的字节。
6. `Finish()`：在解码器排空后，取完重采样器尾部并输出统计。

帧日志中的采样数都是每声道计数。`layout` 的值可能含空格，例如 `1 channels`；这是一种合法布局描述。`delay_samples` 按目标 48000 Hz 采样单位记录，不等于音频设备延迟。

输出格式固定；流中途发生音频参数变化会明确报错。本例不做时间戳断点补静音或精确首尾裁切。写出过程中出错时可能留下不完整的 PCM，报错输出不可当作完整结果使用。

## 运行与试听

在 PowerShell 执行，输出文件必须尚不存在，父目录必须存在：

```powershell
& 'D:\chat_server\ChatServer\lessons\audio_decode\build\audio_decode.exe' 'D:\chat_server\ChatServer\lessons\audio_decode\samples\test_video_with_audio.mp4' 'D:\chat_server\ChatServer\lessons\audio_decode\build\my-audio.pcm'
```

原始 PCM 没有文件头，播放器不能从扩展名判断采样参数。将刚导出的 PCM 封装成 WAV 后可用普通播放器打开：

```powershell
ffmpeg -v error -nostdin -n -f s16le -ar 48000 -ac 2 -i 'D:\chat_server\ChatServer\lessons\audio_decode\build\my-audio.pcm' -c:a pcm_s16le 'D:\chat_server\ChatServer\lessons\audio_decode\build\my-audio.wav'
```

程序已生成的 `decoded.pcm`、`decoded.wav` 可直接查看。实测导出 216 帧，输入每声道 221184 个采样，输出每声道 240745 个采样，共 962980 字节，约 5.015521 秒。重采样器排空阶段取出 17 个采样，计入总输出。

AAC 末尾块含补齐采样，因此实际 PCM 可稍长于 MP4 标称五秒。本例输出与 FFmpeg 命令行逐字节一致，不通过猜测时长截断它。

## 构建

目标 `audio_decode`，Debug / x64，CMake 3.30.5 + Ninja + MinGW 8.1.0。Qt 5.15.2 位于 `F:\Qt\5.15.2\mingw81_64`，FFmpeg 8.1.1 开发包位于 `D:\chat\_deps\ffmpeg-8.1.1-full_build-shared`。链接 avformat、avcodec、avutil、swresample 和 Qt Core。

首次配置或 CMake 输入、依赖位置变化时使用：

```powershell
Set-Location 'F:\Qt\Tools\CMake_64\bin'
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe -S "D:\chat_server\ChatServer\lessons\audio_decode" -B "D:\chat_server\ChatServer\lessons\audio_decode\build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=F:/Qt/Tools/mingw810_64/bin/g++.exe -DCMAKE_MAKE_PROGRAM=F:/Qt/Tools/Ninja/ninja.exe -DCMAKE_PREFIX_PATH=F:/Qt/5.15.2/mingw81_64 -DFFMPEG_ROOT=D:/chat/_deps/ffmpeg-8.1.1-full_build-shared'
```

已有构建目录时增量编译：

```powershell
Set-Location 'F:\Qt\Tools\CMake_64\bin'
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe --build "D:\chat_server\ChatServer\lessons\audio_decode\build" --config Debug --target audio_decode --parallel'
```

2026-10-02 构建结果：**Full build passed**。首次配置约 4.72 秒、首次构建 13.13 秒；补齐显式头文件并格式化后的增量构建 7.52 秒，无编译警告。最终产物 `build/audio_decode.exe` 已更新，929005 字节，修改时间为 2026-10-02 14:15:14（Asia/Shanghai），PE Machine 为 0x8664，即 x64。准确构建命令为上一段。

## 验证

先编写真正处理媒体文件的测试，确认程序缺失时失败，再实现功能。九项测试通过：AAC 平面采样与时间戳、重采样尾部、单声道转换、已知立体声 PCM 字节不变、多音轨选择、中文路径、无音轨、已有输出与输入保护、命令行和缺失路径、无效媒体。部分相关检查组合在同一测试中。

每个成功转换用例都与 FFmpeg CLI 的 S16LE 输出逐字节比较；已知 PCM 用例还直接与生成的采样值比较。帧数、采样数、时间戳和样本格式与 ffprobe 对照。所有预期错误均捕获并断言，正常用例要求 stderr 为空。

在仓库根目录运行：

```powershell
python -X utf8 'lessons/audio_decode/tests/audio_decode_test.py' --tool 'lessons/audio_decode/build/audio_decode.exe' --sample 'lessons/audio_decode/samples/test_video_with_audio.mp4' --silent 'D:/test_video.mp4'
```

测试使用 FFmpeg/ffprobe 8.1.1，并在临时目录生成真实 WAV 和多音轨 MP4。一次测试解析失败的根因是声道布局字符串可含空格；解析按下一个 `字段名=` 定位值边界后通过，未屏蔽 FFmpeg 诊断。

本次验证确认 PCM 数据、封装和资源处理，不包含真实音频设备的播放时钟、暂停或听感验证。
