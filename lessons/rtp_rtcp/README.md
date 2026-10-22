# 10.1：RTP / RTCP 收发实验

两个独立 C++17 程序：`rtp_sender.exe` 从本地文件读取 H.264，通过 UDP 发送；`rtp_receiver.exe` 手动解析 RTP、重组编码数据，用 FFmpeg 解码，再通过 Qt 显示。双方交换 RTCP SR/RR，结束时发送 BYE。

知识文档在本 worktree 的 `docs/音视频学习/10.1-RTP与RTCP.md`。本 README 只记录构建、运行和代码位置。

## 先运行起来

以下命令在 PowerShell 执行，使用这个独立 worktree，原服务端目录仍在 main。

```powershell
$lesson = 'C:\Users\lds\.codex\worktrees\rtp-rtcp\ChatServer\lessons\rtp_rtcp'
```

原 `D:\test_video.mp4` 有 B 帧。先生成教学副本：640×362、25fps、无 B 帧、每秒一个 IDR，不修改原文件。

```powershell
& 'D:\chat\_deps\ffmpeg-8.1.1-full_build-shared\bin\ffmpeg.exe' -hide_banner -loglevel error -y -i 'D:\test_video.mp4' -map 0:v:0 -an -vf 'scale=640:-2,fps=25' -c:v libx264 -preset veryfast -crf 23 -bf 0 -g 25 -keyint_min 25 -sc_threshold 0 -pix_fmt yuv420p -movflags +faststart "$lesson\build\sample-h264.mp4"
```

先在第一个终端运行接收端，会出现黑色画布并等待数据：

```powershell
& "$lesson\build\rtp_receiver.exe"
```

再在第二个终端运行发送端：

```powershell
$lesson = 'C:\Users\lds\.codex\worktrees\rtp-rtcp\ChatServer\lessons\rtp_rtcp'
& "$lesson\build\rtp_sender.exe" "$lesson\build\sample-h264.mp4"
```

默认接收端使用本机 UDP 5004（RTP）、5005（RTCP）；发送端绑定 5006、5007。若端口被占用，两边同时用 `--port 其他端口`，发送端还可用 `--local-port 其他端口`。发送端完成一遍文件后退出，接收端保留最后画面；关闭后重新启动接收端可进行下一场实验。

观察终端：前六包打印 `seq / ts / marker / nal_type / bytes`；随后打印 SR/RR。窗口底部显示接收包数、解码帧数、缺包和重组丢弃数。没有声音是本阶段的范围。

## 观察乱序和丢包

每次实验先重新启动接收端，再执行一条发送命令：

```powershell
# 将部分相邻报文交换发送顺序，接收端应在窗口内还原顺序。
& "$lesson\build\rtp_sender.exe" "$lesson\build\sample-h264.mp4" --reorder-every 7

# 丢掉部分实际 UDP 数据报，同时加入乱序。
& "$lesson\build\rtp_sender.exe" "$lesson\build\sample-h264.mp4" --drop-every 97 --reorder-every 7
```

`--drop-every` 在分配序号后丢弃，因此接收端能看见序号缺口；`--reorder-every` 保持序号不变，只交换提交 socket 的先后。它们改变真实发送，不伪造接收回调，也不修改服务器网络设置。

缺包等待超过 50ms 后，日志出现 `RTP gap`；损坏访问单元不送解码器，直到下一份完整 IDR 才恢复。修改丢包间隔可能把每份 IDR 都打坏，画面便可能一直等待；RTCP RR 只报告损失，本例没有 NACK 重传或 PLI 请求关键帧。

## 代码阅读顺序

| 顺序 | 文件 | 重点 |
|---|---|---|
| 1 | `rtp.h` | 一个 RTP 包的字段，以及重排/重组对象分别保存什么 |
| 2 | `sender.cpp` 的 `FileReader::Next`、`Sender::Pump` | MP4 解封装、Annex B 转换、按媒体时刻发送 |
| 3 | `rtp.cpp` 的 `Packetize`、`EncodeRtp` | NAL 拆成 Single NAL / FU-A，再加 12 字节 RTP 头 |
| 4 | `receiver.cpp` 的 `ReadRtp`、`Drain` | 到达统计、序号重排、缺包处理、完整单元交给解码器 |
| 5 | `rtp.cpp` 的 `H264Assembler::Push` | Single NAL / STAP-A / FU-A 怎样恢复成 Annex B |
| 6 | `video_decoder.cpp`、`receiver.cpp` 的 `Queue`、`Tick` | 编码单元解码成 RGB，然后按时间显示 |
| 7 | `rtcp.h/.cpp`、双方的 `Report` 和读报告函数 | SR/RR、统计单位、LSR/DLSR 和 RTT |

`wire.h` 只提供网络字节序读写和计数回绕展开；`ffmpeg_support.h` 只提供 FFmpeg 错误转换与资源释放。画布复用 `../video_player/video_view.cpp`。没有改聊天业务代码、Qt 主客户端、SRS 或云端服务。

## 构建

已确认本机使用 Qt 5.15.2、MinGW 8.1 x64、CMake/Ninja、FFmpeg 8.1.1 SDK。该目录是独立 CMake 工程，不从 ChatServer 顶层构建。

首次配置：

```powershell
Set-Location 'F:\Qt\Tools\CMake_64\bin'
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe -S "C:\Users\lds\.codex\worktrees\rtp-rtcp\ChatServer\lessons\rtp_rtcp" -B "C:\Users\lds\.codex\worktrees\rtp-rtcp\ChatServer\lessons\rtp_rtcp\build" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=F:/Qt/Tools/mingw810_64/bin/g++.exe -DCMAKE_MAKE_PROGRAM=F:/Qt/Tools/Ninja/ninja.exe -DCMAKE_PREFIX_PATH=F:/Qt/5.15.2/mingw81_64 -DFFMPEG_ROOT=D:/chat/_deps/ffmpeg-8.1.1-full_build-shared'
```

增量构建两个程序：

```powershell
cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;%PATH%" && cmake.exe --build "C:\Users\lds\.codex\worktrees\rtp-rtcp\ChatServer\lessons\rtp_rtcp\build" --config Debug --target rtp_sender rtp_receiver --parallel 2'
```

EXE 旁的 Qt/FFmpeg DLL、`platforms/` 是运行依赖，需要一并保留。更改 `.cpp/.h` 后直接增量编译；只有 CMake 输入或工具链变化才需要重新配置。

## 必要验证

`protocol_check` 用固定报文字节检查协议字段、FU-A/STAP-A、丢片、重排、回绕和 SR/RR 单位，不默认加入普通构建。需要运行验证时，先在构建命令的目标中追加 `protocol_check`，再执行：

```powershell
Set-Location 'C:\Users\lds\.codex\worktrees\rtp-rtcp\ChatServer'
python 'lessons\rtp_rtcp\verify.py' --build 'lessons\rtp_rtcp\build' --sample 'lessons\rtp_rtcp\build\sample-h264.mp4' --ffmpeg 'D:\chat\_deps\ffmpeg-8.1.1-full_build-shared\bin\ffmpeg.exe'
```

验证脚本针对上面的 5 秒、125 帧教学副本；包括真实双进程 UDP 正常播放、丢包/乱序、首帧像素对照、FFmpeg→Qt 与本程序→FFmpeg 两个方向的互通。实际日志、首帧和窗口截图保存在 `build/verification/`。双向互通使协议格式不只依赖自有两端相互认可。

2026-10-18 验证通过：正常解码 125 帧、无 RTP 缺口；丢包实验确认 2 包丢失并两次恢复；与外部 FFmpeg 双向互通，125 幅 YUV 解码帧哈希全部一致。Windows 实际窗口显示正常。完整构建命令、产物时间和验证边界记录在 `docs/STAGE10_RTP_DELIVERY.md`。

## 实现范围

单场、单 H.264 视频源、单播 IPv4、PT=96、无 B 帧；发送 Single NAL/FU-A，接收额外支持 STAP-A。参数集通过码流提供，SDP 中动态负载的约定需要双方一致。默认只在本机演示。

每份访问单元上限 8MiB，乱序队列至多 256 包；画面队列至多 32 张且至多 32MiB。初始显示缓冲 100ms，缺包等待 50ms。接收端在 Qt 事件循环做轻量解码，适合本教学副本，不是生产播放器的线程模型。

RTCP 使用教学用一秒周期，未实现参与者变化后的完整定时重考虑、SSRC 冲突处理或发送源重启判定。没有音轨、重传、拥塞控制、鉴权、加密或 NAT 穿透。10.2 的摄像头 WebRTC 会使用成熟协议库。
