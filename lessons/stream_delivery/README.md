# 流媒体分发实验

本目录承载加固阶段 4～6。每个入口运行真实服务与真实媒体请求，生成独立配置和记录，只清理自己启动的进程。代码不更改正式 SRS/Nginx 配置，不访问生产直播数据。

知识正文位于统一学习目录：

- [加固 04：直播接入与多观众分发](D:/chat_server/ChatServer/docs/音视频学习/加固04-直播接入与多观众分发.md)
- [加固 05：HLS 资源发布与缓存](D:/chat_server/ChatServer/docs/音视频学习/加固05-HLS资源发布与缓存.md)
- 阶段 6 的实际业务环境、运行入口及结果见 [LIFECYCLE.md](LIFECYCLE.md)。

## 环境与资源归属

在 Linux/WSL 中运行 Python 3。需要 FFmpeg（libx264、AAC）、`ss`、固定的 SRS 6.0.184、支持 `http_auth_request` 的 Nginx。脚本不安装依赖、不关闭其他程序；端口已被占用时直接报告错误。

本机已核实的依赖：

| 项目 | 本机路径或版本 |
|---|---|
| WSL | `ChatUbuntu`，用户 `lth` |
| SRS | `/mnt/d/chat/_server/Output/linux-x64/rtc/srs`，6.0.184，沿用现有 SR 补丁产物 |
| Nginx | `/opt/chat-build/deps/runtime-tools/usr/sbin/nginx`，1.24.0 |
| FFmpeg | `/usr/bin/ffmpeg` |
| Python | `/usr/bin/python3` |
| 代码入口 | `/mnt/c/Users/lds/.codex/worktrees/rtp-rtcp/ChatServer/lessons/stream_delivery/` |

不传 `--output` 时，输出放在 `~/.local/state/chat-stream-lessons/` 下的随机子目录。指定 `--output` 时，该目录必须尚不存在，避免覆盖已有记录。运行失败同样保留配置、命令和日志。生成的 MP4/TS、缓存和日志不应加入源码提交。

运行包含子进程组归属：`Process` 只向本次 `Popen` 启动的进程组发送终止信号，等待退出；超过 8 秒才结束该组。正常完成或 Python 异常退出均走同一资源释放路径。不要用宽泛的 `pkill srs` 或关闭 WSL 来代替本次实例清理。

## 阶段 4：一位慢观众与一位正常观众

在 PowerShell 中运行：

```powershell
wsl -d ChatUbuntu -u lth -- python3 /mnt/c/Users/lds/.codex/worktrees/rtp-rtcp/ChatServer/lessons/stream_delivery/delivery.py
```

Linux 内可直接执行：

```bash
python3 lessons/stream_delivery/delivery.py --output /home/lth/delivery-observation
```

默认 `--base-port 21980` 分配以下环回端口：

| 用途 | 端口 |
|---|---:|
| SRS HTTP-FLV | 21980 |
| 两路真实 HTTP 中转 | 21981 |
| SRS HTTP API | 21985 |
| SRS RTMP 发布 | 21986 |

实验生成 8 秒可循环的 1280×720、30 fps、H.264/AAC 素材。视频目标码率 8 Mbps，GOP 30、无 B 帧。发布端以真实时间节奏循环推入 RTMP，两个观测器同时通过 HTTP-FLV 加入。

正常观众不限制读取速率；慢观众前 25 秒最多按 32768 字节/秒安排下一次上游读取，然后取消限速。实际低于上限也可能是协议调度或网络造成，脚本不把上限等同于实测吞吐。观测持续 45 秒，随后退出两位观众，等待 SRS API 中只剩发布连接，再关闭本次服务。

中转对每条连接只循环使用一个最多 4096 字节的数据块，没有后台无限预读。socket 请求的收发缓冲均固定为 256 KiB；本机 Linux 实际返回 512 KiB。缓冲固定，避免取消限速时依赖 TCP 接收窗口动态扩大的行为。8 Mbps 输入和 25 秒限速持续时间足够填满有限传输缓冲，从而观察到 SRS 的真实背压。

SRS 实验配置使用 2 秒消费者队列并启用已有 trace 日志。正式配置没有变化，SRS 源码也没有为本实验新增补丁。

### 输出如何读

| 文件 | 口径 |
|---|---|
| `commands.jsonl` | 精确命令、PID、退出码和耗时 |
| `result.json` | 观众统计、DTS 最大间隔、队列清理次数 |
| `normal.jsonl`、`slow.jsonl` | 约每秒累计 FLV 字节、AVC 视频 tag 数和媒体 DTS |
| `relay.jsonl` | 每路上游/下游累计字节、限速阶段、实际 socket 缓冲及上游本地端口 |
| `server.jsonl` | 同时存在的 SRS 连接身份、字节统计、SRS RSS |
| `sockets.txt` | 每秒 `ss -tinm`，将对端端口与 relay 的上游本地端口对应 |
| `srs.log` | 固定版本已有的 `shrinking`、连接和关闭日志 |
| `clients-after-viewers.json` | 两位观众退出后只剩发布连接的实测快照 |

`first_video_after_s` 指首个完整 AVC 视频媒体 tag 到达的时间，不是首帧解码或屏幕显示时间。`video_tags` 同样不是渲染帧数。该观察器只解析本实验的普通 FLV/H.264 格式，不用于替代通用解封装器。

通过条件包括：正常观众媒体持续推进且最大 DTS 间隔不超过 100 ms；实际出现 SRS 队列清理；慢观众恢复后观察到大于等于 1 秒的媒体时间跳跃，最后追到正常观众的 1.5 秒范围内；两位观众取消后连接被释放。出现未预期失败时保留材料并报错，不把它归为一次成功演示。

### 单独使用中转

```bash
python3 lessons/stream_delivery/relay.py \
  --origin http://127.0.0.1:21980/live/lesson.flv \
  --port 21981 --slow-bytes-per-second 32768 --slow-seconds 25 \
  --events /home/lth/relay-events.jsonl
```

观看 `/normal` 或 `/slow`。上游严格限定为一个固定的 `127.0.0.1` HTTP 路径，不从请求参数接受其他目标。中转不转发业务票据；它用于本目录无业务鉴权的隔离 SRS，不能拿这个路径绕过正式直播授权。

## 阶段 5：发布、保留、缓存与授权

```powershell
wsl -d ChatUbuntu -u lth -- python3 /mnt/c/Users/lds/.codex/worktrees/rtp-rtcp/ChatServer/lessons/stream_delivery/hls.py
```

默认源站 `127.0.0.1:21982`，Nginx 边缘 `127.0.0.1:21983`。`--base-port` 可整体调整；`--nginx`、`--ffmpeg` 可指定现有二进制。

源站实际提供 FFmpeg 生成的 H.264/AAC TS 文件。脚本改变独立目录中的列表和文件状态，Nginx 直接按真实响应缓存：

1. 正常：完整片段先生成，再发布列表；实际请求所得片段经过 FFmpeg 解码，错误日志必须为空。
2. 引用过早：列表已经引用 `pending.ts`，文件尚不存在，真实请求 404；补齐文件后 200。
3. 列表过旧：边缘缓存 5 秒，源站从媒体序号 0 推进到 1，边缘 HIT 仍返回 0，过期后 EXPIRED 返回 1。
4. 淘汰过早：观众保存旧列表；源站推进列表并删除旧片段；观众请求旧引用得到真实 404。
5. 缓存与授权：受保护片段首次 MISS、再次 HIT；短期签名票据到期后 401；更换有效票据仍 HIT。四次授权请求，只发生一次片段回源。

最后一项使用本地独立签名与过期策略，验证 Nginx 的 `auth_request` 与缓存关系；它没有冒充 ChatServer 登录、房间权限或五分钟业务票据。业务票据的真实到期实验由阶段 6 完成。

输出包含：

- `responses/*.json`：每次状态码、响应头、源站资源请求数、授权请求数、实体摘要。
- `responses/*.body`：实际返回的列表、片段或错误实体。
- `origin.jsonl`：源站访问时间、路径、状态，不记录原始票据。
- `nginx.conf`、`nginx-access.log`、`nginx-error.log`：本次配置与真实代理记录。
- `result.json`：全部观察及允许出现的预期错误标签，只有特定的 404/401 属于实验预期。

Nginx 的所有临时目录、缓存、PID 与日志均落在本次输出目录，运行不依赖 `/var/lib/nginx` 写入权限。

## 验证记录与范围

2026-10-22，本机 `delivery.py --output /home/lth/stream-delivery-20261022-4` 返回 0，实测如下：

| 指标 | 结果 |
|---|---:|
| 正常观众 AVC 视频 tag 数 | 1347 |
| 正常观众最大相邻视频 DTS 间隔 | 34 ms |
| 慢观众 AVC 视频 tag 数 | 747 |
| 慢观众恢复时最大 DTS 跳跃 | 20100 ms |
| 结束时正常/慢观众 DTS | 44877 / 44943 ms |
| SRS 队列清理 | 10 次 |
| SRS 最大实际 TCP Send-Q | 2648963 字节 |
| 观测期间 SRS RSS 范围 | 17764～20120 KiB |
| 慢观众 24.177 秒时累计上游读取/下游发送 | 792760 / 792760 字节 |
| 实际解除限速时刻 | 25.000 秒 |

两位观众释放后 API 断言只剩发布连接。生成器、发布端和中转日志为空；SRS 的 8 条启动/关闭警告已逐条分类，`expected-srs-warnings.json` 保存原文，未出现未预期错误。SRS 禁用的 HLS/DASH 模块关闭时仍可能输出空路径清理警告，不能据此声称实验在使用 HLS/DASH。

开发中的一次恢复检查没有通过：把慢连接接收缓冲从 4 KiB 改大后，已建立 TCP 连接的实际广告窗口仍为 8192 字节，解除用户态限速后吞吐仍偏低。当前中转使用固定的有限缓冲，并通过 Send-Q、队列日志和媒体时间三组证据确认压力与恢复；没有把未恢复的运行记为通过。

2026-10-22，本机最终验证命令 `hls.py --output /home/lth/stream-hls-20261022-4` 完成全部场景，进程返回 0。正常列表响应类型为 `application/vnd.apple.mpegurl`，TS 片段为 `video/mp2t`，两者均有断言；正常片段 FFmpeg 解码错误日志为空。源站访问与 Nginx 缓存状态符合上述五组预期，所有子进程退出码为 0，端口已释放，日志仅有 6 条 Nginx notice。`passed: true` 在清理和日志审查全部完成后写出。

一次开发运行的 Nginx 配置检查发现其编译默认的 fastcgi 临时目录不存在；本目录配置已把 proxy/client/fastcgi/uwsgi/scgi 临时目录全部指向本次输出位置。该问题属于隔离环境配置，不涉及正式服务。

阶段 4 与阶段 5 最终运行的小体积记录另存于本目录 `build/delivery-20261022/` 与 `build/hls-20261022-review2/`，便于在 Windows 工作目录查看；阶段 5 此前记录保留在 `build/hls-20261022/`、`build/hls-20261022-review/`。未复制大体积生成视频。`build/` 属于忽略的本地实验产物。

阶段 4 的 TCP 限速实验不代表 RTC 丢包、NACK、PLI 或公网网络验证。阶段 5 的单边缘实验也不代表完整 CDN、低延迟 HLS 或多档自适应切换。三阶段共用方法，但每个结论保留其实际观察边界。
