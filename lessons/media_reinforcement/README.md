# 媒体加固实验：阶段 1～3

这里复用 `media_info`、`media_process` 和 Qt 客户端。脚本只启动本次命令对应的本地子进程，不访问云服务器、不修改系统网络；正常结束后没有常驻进程。Ctrl+C 可中断当前脚本，半途输出保留用于定位；下一次使用新的输出目录。

知识正文位于唯一学习目录 `D:/chat_server/ChatServer/docs/音视频学习`：`加固01-格式能力与正确显示.md`、`加固02-时间线时钟与缓冲.md`、`加固03-编码封装与分片边界.md`。本 README 保存命令、资源与观察证据。

## 生成共用素材

以下 PowerShell 命令在后端仓库根目录执行。本次工作目录是 `C:/Users/lds/.codex/worktrees/rtp-rtcp/ChatServer`。需要 FFmpeg/ffprobe 8.1.1 或兼容相应选项的版本，带 libx264 与 AAC 编码器；本机使用既有 SDK 中的程序。

```powershell
python lessons/media_reinforcement/generate_samples.py lessons/media_reinforcement/build/samples --ffmpeg D:/chat/_deps/ffmpeg-8.1.1-full_build-shared/bin/ffmpeg.exe --ffprobe D:/chat/_deps/ffmpeg-8.1.1-full_build-shared/bin/ffprobe.exe
```

目标目录必须不存在；已有 `samples` 可直接复用，重做时指定另一目录。脚本将 FFmpeg 的错误输出当成失败并保存 `commands.json`；成功后还保存 `manifest.json`（文件大小、SHA-256、真实轨道参数）。不会下载媒体、改写原始素材或安装依赖。

| 文件 | 内容与观察目标 |
|---|---|
| `base.mp4` / `base.mkv` | 同一 H.264/AAC 编码内容的两种封装；854×480、八秒、30 fps |
| `sar_4_3.mp4` | SAR 4:3，未旋转 DAR 为 427:180 |
| `rotate_90.mp4` / `rotate_180.mp4` / `rotate_270.mp4` | 显示矩阵方向；参数角度采用 FFmpeg 的逆时针语义 |
| `rotate_90_sar.mp4` | 同时存在 SAR 与旋转，验证几何组合顺序 |
| `mirror.mp4` | 水平镜像矩阵；只读取旋转角度无法完整表达 |
| `rotate_45.mp4` | 非正交矩阵，用于验证播放器明确拒绝超出范围的变换 |
| `audio_late.mp4` | 名义音轨偏移 0.75 秒；实际首包/轨道起点受 AAC priming 与容器处理影响，读取 PTS 后解释 |
| `audio_gap.mkv` | 删除 2～3 秒的音频编码输入，保留时间戳空洞；没有把后续音频提前 |
| `vfr_bframes.mp4` | 前半段选择每三帧之一，后半段全部保留；保留真实 PTS，并允许 B 帧 |

未经显示变换的四角各有 160×120 色块：左上红、右上绿（lime）、左下蓝、右下黄。中部为运动图案。Qt 验证可据此核对旋转、镜像和黑边，不需要字体或人工辨认字幕。

这些素材用于软件行为验证。正弦波不能代表主观听感或真实设备采集质量，八秒视频也不能代表长期内存稳定性。

## 编码与分片对照

构建两个现有工具后执行：

```powershell
python lessons/media_reinforcement/run_media_lab.py lessons/media_reinforcement/build/samples lessons/media_reinforcement/build/run-20261022 --media-process lessons/media_process/build/media_process.exe --media-info lessons/media_info/build/media_info.exe --ffmpeg D:/chat/_deps/ffmpeg-8.1.1-full_build-shared/bin/ffmpeg.exe --ffprobe D:/chat/_deps/ffmpeg-8.1.1-full_build-shared/bin/ffprobe.exe
```

输出目录必须不存在。脚本完成这些有区分度的真实操作：

1. 读取格式、轨道、SAR、颜色、矩阵等属性。
2. 把同一 MP4 转封装为 TS、MKV，完整解码并比较 240 幅视频像素摘要。
3. 对照 GOP 一秒/B 帧 0 与 GOP 三秒/B 帧上限 2；两档都限定最大码率 900 kbit/s、CRF 23。该对照同时改变两个参数，用于看清结构差异，不用于单变量性能结论。
4. 生成 TS 与 fMP4 HLS，完整解码两档列表，再解码每档首尾代表分片。fMP4 使用该档 `init.mp4` 加目标 `.m4s`。
5. 读取 TS 第一视频 packet 的真实字节，识别 Annex B NAL；记录真实帧类型、关键帧 PTS、前几个编码包 PTS/DTS。
6. 核对已有输出保护、越界参数、CRF 0 与非 AAC 音频的明确拒绝；预期错误单独记录并验证，不混入正常路径日志。

`commands.json` 记录每条真实命令、耗时、退出码、stdout/stderr 和预期错误；`observations.json` 汇总结构。正常产物不覆盖源文件，源摘要在结束时复核。

## 工具的受控参数

```powershell
lessons/media_process/build/media_process.exe remux lessons/media_reinforcement/build/samples/base.mp4 lessons/media_reinforcement/build/example.ts
lessons/media_process/build/media_process.exe transcode lessons/media_reinforcement/build/samples/base.mp4 lessons/media_reinforcement/build/example-encode --gop-seconds 3 --b-frames 2 --crf 23 --max-rate-kbps 900
```

- TS：一条 H.264 普通视频，至多一条 AAC 音轨。字幕、附件、封面跳过；不提供容器显示矩阵的交付契约，旋转素材不会自动旋转像素。
- GOP 1～10 秒按名义帧率换算最大帧数；VFR 的真实关键帧间隔需读取 PTS。
- B 帧 0～8 为上限；实际分布由编码器决定。
- CRF 1～51。CRF 0 的无损行为会使 x264 忽略当前 VBV，所以本工具拒绝该组合。
- 最大码率 64～20000 kbit/s。省略时 480p/360p 分别使用已有 1200/700 kbit/s 上限。此参数是 VBV 上限，不是平均码率保证。
- HLS 命令只接受 `ts` 或 `fmp4`，继续使用共同两秒 IDR 边界，不能传入上述实验编码选项。

## 本机配置与构建

本次 RTP 工作目录没有两个工具的构建缓存，所以首次配置独立构建目录；没有移动或复用指向 main 源码的旧缓存。工具链沿用 Ninja、MinGW 8.1 x64、Qt 5.15.2、FFmpeg 8.1.1。以下命令在 `F:/Qt/Tools/CMake_64/bin` 执行。

```powershell
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe -S C:/Users/lds/.codex/worktrees/rtp-rtcp/ChatServer/lessons/media_info -B C:/Users/lds/.codex/worktrees/rtp-rtcp/ChatServer/lessons/media_info/build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=F:/Qt/Tools/mingw810_64/bin/g++.exe -DCMAKE_MAKE_PROGRAM=F:/Qt/Tools/Ninja/ninja.exe -DCMAKE_PREFIX_PATH=F:/Qt/5.15.2/mingw81_64 -DFFMPEG_ROOT=D:/chat/_deps/ffmpeg-8.1.1-full_build-shared'
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe -S C:/Users/lds/.codex/worktrees/rtp-rtcp/ChatServer/lessons/media_process -B C:/Users/lds/.codex/worktrees/rtp-rtcp/ChatServer/lessons/media_process/build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=F:/Qt/Tools/mingw810_64/bin/g++.exe -DCMAKE_MAKE_PROGRAM=F:/Qt/Tools/Ninja/ninja.exe -DCMAKE_PREFIX_PATH=F:/Qt/5.15.2/mingw81_64 -DFFMPEG_ROOT=D:/chat/_deps/ffmpeg-8.1.1-full_build-shared'
```

日后仅源码变化时使用缓存中的 CMake 做增量构建，不重新配置。内存紧张时两个目标顺序执行：

```powershell
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe --build C:/Users/lds/.codex/worktrees/rtp-rtcp/ChatServer/lessons/media_info/build --config Debug --target media_info --parallel'
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe --build C:/Users/lds/.codex/worktrees/rtp-rtcp/ChatServer/lessons/media_process/build --config Debug --target media_process --parallel'
```

这是 Ninja 后端，不传 MSBuild 属性。构建命令本身不运行实验；上面的实验命令是独立入口。

## 2026-10-22 代表结果

首次 `media_info` Debug x64 构建 **Full build passed**，11.299 秒。`media_process` 首次并行构建因 `cc1plus.exe: out of memory allocating 65536 bytes` 失败；检查无残留构建进程，当时可用提交空间约 1.1 GiB。相同参数增量编译剩余一个对象并链接，4.571 秒 **Full build passed**，没有更改工程、工具链、输出路径或终止其他程序。

最终 CRF 边界与未知 level 显示完成后，按上面的相同命令顺序构建：`media_process` 15.194 秒、`media_info` 11.274 秒，均 **Full build passed**、日志无警告。构建期间编码保持检查更新了文件时间，因此 `media_process` 重新编译其五个对象；未执行 Clean/Rebuild。产物信息保存在 `build/build-artifacts.json`：

| 产物（相对后端根目录） | 大小 | 修改时间（北京时间） | 架构 |
|---|---|---|---|
| `lessons/media_info/build/media_info.exe` | 673,880 字节 | 2026-10-22 20:51:02 | PE 0x8664 / x64 |
| `lessons/media_process/build/media_process.exe` | 3,229,689 字节 | 2026-10-22 20:50:41 | PE 0x8664 / x64 |

`build/run-20261022` 的代表实验结果：

| 项目 | 实际结果 |
|---|---|
| 转封装视频像素 | TS、MKV 均与原件 240 帧摘要相同 |
| TS 第一个视频 packet | NAL 类型为 9、6、7、8、5，即 AUD、SEI、SPS、PPS、IDR |
| 480p GOP 一秒/B0 | 8 I、232 P；关键帧 PTS 为 0～7 秒每秒一个；1,125,200 字节 |
| 480p GOP 三秒/B2 | 3 I、152 B、85 P；关键帧 PTS 为 0、3、6 秒；1,061,370 字节 |
| PTS/DTS | 无 B 帧样本开头两者相等；B 帧样本初始 DTS 为 -0.066667 秒，PTS 与 DTS 顺序不同 |
| TS 与 fMP4 HLS | 各有 480p/360p 两档，每档四片、每片两秒；列表和首尾代表片均完整解码成功 |
| 原件保护 | 原件 SHA-256 保持 `7352d8ef906e7f2411536babb3d9c163f096168c2aeb05b6cf10d90f88f43bda` |

正常路径 FFmpeg 解码采用 `-v error -xerror` 且无错误输出。首次实验 40 个实际命令累计执行约 11.566 秒。最终二进制补充验证 CRF 0、非 AAC 音频、重复选项三类拒绝，单行错误逐项核对、没有生成目标或 `.part`，记录位于 `build/boundary-verification.json`；`media_info` 对 AAC 未知 level 输出 `unknown`。后续再次生成的编码结果可能受版本、CPU 与实现变化影响，不把这里的文件大小当成所有平台的固定断言。

TS 自动 BSF 的判断结合固定 FFmpeg 版本封装器行为与真实产物；实验没有显式调用 `h264_mp4toannexb`。AAC 首尾无缝裁剪、旋转元数据保真、所有编码格式支持不在这份 TS 转封装验证的结论内。
