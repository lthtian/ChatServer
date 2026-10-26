# 直播生命周期实验

入口 `lifecycle.py` 运行真实 MySQL、独立 Redis、ChatServer、SRS、Nginx、FFmpeg 发布和观看链路。它复用 `tests/media_service_test.py` 的随机数据库与最小账号夹具；只有本次创建的进程、数据库、账号和临时目录会被清理。默认完整实验会等待实际五分钟票据到期。

## 本机运行

本机已配置 WSL `ChatUbuntu`、MySQL 8、Redis、FFmpeg。依赖默认路径是现有 RTC 构建产物，命令行也可以显式指定 `--server`、`--srs`、`--nginx`。不要指向云端业务环境。

```powershell
wsl.exe -d ChatUbuntu -u root --exec python3 /mnt/c/Users/lds/.codex/worktrees/rtp-rtcp/ChatServer/lessons/stream_delivery/lifecycle.py
```

使用本机 root 仅为复用 MySQL socket 管理员认证和创建隔离夹具。管理员若已配置非交互 sudo，也可用普通 WSL 用户加 `--sudo`；本机当前 `lth` 的 `sudo -n` 未获认证，不能直接假定该方式可用。脚本不会修改 MySQL 认证规则或安装依赖。

随机选择本机端口；配置和媒体文件放 `chat-lifecycle-*` 临时目录。每次保存到 `lessons/stream_delivery/build/lifecycle-<时间>/`，包括 `result.json` 和脱敏日志。不要同时启动两份同一秒的实验，输出目录采用排他创建防止覆盖。

`--skip-expiry` 只供定位其他场景，结果会明确标记 `expiry_verified=false`；不能用它声称已完成到期验证。

## 场景与证据

| 场景 | 注入方式 | 必须看到的结果 |
|---|---|---|
| 迟到退出回调 | 暂缓转发 SRS 实际发出的退出请求，在另一连接发布后释放 | 记录不同发布 ID，回调真实转发 200，业务仍属于当前连接 |
| 五分钟截止 | 保持原 HTTP-FLV 响应，等待真实票据截止 | 原票据新建 FLV/HLS 请求均 403；已有 FLV 字节仍增加；续签后 HLS 200 |
| 观看恢复 | 使用新的真实 HLS 描述进行音视频解码 | FFmpeg 完整解码两秒，退出 0 且无非预期诊断 |
| 业务结束而断开失败 | 管理中转关闭真实 DELETE 连接，GET 仍正常 | 业务 ended，新请求 403，发布者与旧响应暂仍推进 |
| 状态核对恢复 | 允许管理 DELETE 通过 | 发布进程退出，发布标识清空，业务仍 ended |

控制中转只转发固定本机地址。不改回调正文，不伪造成功响应，不修改票据期限。媒体流量不经过控制中转，HTTP-FLV 观察线程持续读取，内存只保留字节计数。

正常日志与预期故障分开核对：`publisher disconnect deferred` 是管理连接被关闭的预期结果；结束后 FFmpeg 推流可能报告 Broken pipe / 写尾失败，这来自主动终止媒体接入。其他错误需要解释，不能仅以最终布尔值代替检查。

## RTC 会话释放验证

RTC 使用现有 `tests/rtc_service_test.py`，要求 Qt `build/rtc-smoke/debug/rtc_smoke.exe` 已由当前源码构建，Qt 插件、FFmpeg、libdatachannel DLL 及 `sample.png` 已按该目标配置。

```powershell
wsl.exe -d ChatUbuntu -u root --exec python3 /mnt/c/Users/lds/.codex/worktrees/rtp-rtcp/ChatServer/tests/rtc_service_test.py
```

该入口同样创建独立数据库和服务。两个 Windows 进程用合成画面及编码音频通过真实 SRS 收发，观察解码帧、PCM 和时间差；窗口路径还经过共用画布与音频输出。观看端正常退出后，检查 SRS 只剩发布者；两次关闭都必须出现 `DELETE 204`。业务结束后再检查剩余连接回收。

`build/rtc-smoke/rtc-methods.log` 仅保存 HTTP 方法和状态，不保存 SDP、Authorization 或会话资源路径。日志中的 `POST 403` 是无凭证请求的预期拒绝；正常协商为 201，关闭为 204。

这个实验不能证明真实相机驱动、麦克风设备、NAT 穿透或主观听感。Windows 进程通过自身限时 Stop 退出，不能用结束 cmd 父进程代替媒体线程回收。

## 结果解释

`result.json` 的 `passed` 只表示这次配置与样本下各断言满足；`expiry_verified` 单独说明是否等待了实际期限。`elapsed_s` 是从实验开始累计的单调时间，`established_flv_delta` 是观察窗口内收到的字节增量。累计字节数、HTTP 成功、业务 live 与解码成功分别说明不同事实。

详细知识正文在固定学习目录的《加固06-直播生命周期授权与恢复》，实际执行记录汇总在《六阶段加固交付记录》。

## 本次日志的逐项审查

以下三个输出目录保存 `log-audit.json`。审查基于已经完成的真实运行，没有再次启动服务；每份记录保存输入文件 SHA-256、捕获诊断的文件与行号、分类、计数、检查结果及源码依据。未匹配的诊断会进入 `unknown_diagnostics` 并使审查不通过，不以场景的 `passed` 替代日志检查。

| 运行 | 审查文件 | 结果 |
|---|---|---|
| 实际五分钟票据到期 | `build/lifecycle-20261022-205111/log-audit.json` | 通过，`expiry_verified=true`，未知诊断 0 |
| 跳过到期等待的局部复核 | `build/lifecycle-20261022-205419/log-audit.json` | 通过，`expiry_verified=false`，未知诊断 0 |
| RTC 收发、窗口及释放 | `E:/chat_server_qt/ChatClient-rtc/build/rtc-smoke/log-audit.json` | 通过，未知诊断 0 |

两次生命周期审查核对了 `result.json` 中的不同发布连接 ID、管理请求故障、ended 收敛与发布身份清空，并将诊断绑定到这次运行的对象：

- SRS 主动踢出的 ID 必须等于重推后的发布连接；其后恰好一条 `1070(StThreadInterrupt)` 错误和四行对应调用栈，属于管理踢出中断发布协程。不能把任意 1070 自动当成成功。
- Nginx 恰好一条已建立 FLV 响应的上游提前关闭，URI 必须属于本次 `live_id`；它对应媒体接入被撤销后的响应结束。
- 第二个发布者恰好四条连接重置、封装写入失败、写尾失败及 Broken pipe 诊断；第一个主动重推发布者的日志为空。
- 业务端恰好一条 `publisher disconnect deferred`，对应本次实际关闭的管理 DELETE 连接；它与稍后的状态收敛一起判断，单独出现不能证明恢复成功。
- 完整运行的访问日志为 7 次 200、4 次 403，局部复核为 7 次 200、2 次 403。403 的路径属于本次流；完整运行另外核对真实等待超过 300 秒、旧票据新请求拒绝且已有响应继续增加字节。
- SRS 对端 socket 关闭诊断分别为 111、11 条，固定版本 HTTP 连接代码将 1007/1009 归入对端关闭路径；其余启动统计与自有实例关闭诊断逐项捕获。

Redis 的 `Memory overcommit must be enabled` 在三个运行中各一条。这是现有主机设置提示，已记录，没有修改内核参数；本次通过不代表 Redis 在低内存、后台持久化或压力场景下已经验证。`processor is not configured` 对应本次夹具未启用离线媒体处理器，不影响正在验证的直播链路。

### `VIDEO: Timestamp … may need mix_correct` 的准确含义

固定源码位置为 `/opt/chat-build/deps/srs-v6.0-r0/trunk/src/app/srs_app_source.cpp`：

- `2243～2250`：`on_audio()` 比较并更新 `last_packet_time`。
- `2377～2384`：`on_video()` 使用同一个 `last_packet_time` 比较并更新。
- `2600～2601`：每次发布重新初始化这组检测状态。第一次发现下降后，`is_monotonically_increase` 设为 false，不会据此持续重复计数。

因此这条日志比较的是**音视频共同的接收次序时间戳**。`VIDEO` 仅表示当前进入 `on_video()`；前一个更新时间戳的消息可能是音频。检查发生在 `on_frame()` 和 GOP 缓存之前，不能称为“GOP 缓存时标告警”，也不能直接解释为“相邻两帧视频 DTS 倒退”。这一入口尚未区分普通视频媒体、配置或收尾消息。

完整运行捕获 `618=>581`，归属首次发布 ID `12a401x9`；局部复核捕获 `576=>541`，归属首次发布 ID `936819kt`。它们出现在开始后很快终止首次发布并重推的场景。音频的 AAC 采样步进、25 fps 视频的步进及停止时封装收尾，均可以解释跨轨道消息时序的这种差异。

**证据边界**：保留的日志没有逐包类型和时间戳，无法证明被比较的前一个包一定是音频，也不能仅凭它证明每条轨道都严格单调。审查将这两条精确值和连接归属记为已解释其检测口径的观察，不声称已经排除了所有时间线问题；它们不作为业务状态测试失败，也没有触发无依据的 `mix_correct` 配置修改。

### RTC 的 `DTLS_HANG` 与最后一个无会话数据报

固定源码 `/opt/chat-build/deps/srs-v6.0-r0/trunk/src/app/` 中：

- `srs_app_rtc_dtls.cpp:403～407` 在 **DTLS 对象析构**时检查 `handshake_done_for_us`，为 false 才打印 `DTLS_HANG`。这是销毁时的状态记录，不是运行中的超时检测回调。
- `srs_app_rtc_network.cpp:39～65` 为每个 RTC 连接同时创建并初始化 UDP 与 TCP 网络对象，两者各自拥有安全传输和 DTLS 对象（`149～163`、`437～451`、`189`、`570`）。只使用 UDP 时，另一个未使用的传输对象可以一直未完成握手，释放时同样输出该日志。
- `srs_app_rtc_dtls.cpp:462～470` 表明 `version=-1` 是自动版本选择，不等于协商出一个非法版本。成功完成握手后在 `610～612` 设置完成状态。
- `srs_app_rtc_api.cpp:644～660` 的会话 DELETE 触发 `expire()`；`srs_app_rtc_conn.cpp:1957～1960` 将会话从管理器移除，随后释放网络对象。

本次恰好三条 `done=0, version=-1, arq=0`，对应两个观看会话和最终发布会话的释放阶段。独立的 HTTP 记录为一次预期 POST 403、三次 POST 201、三次 DELETE 204；解码观察为 406 个视频帧、991 个音频批次、结束时音视频差 61356 µs，窗口实际呈现 395 帧。这些媒体和释放证据与源码共同支持把本次三条诊断归入对象销毁观察。

**不能扩大结论**：日志没有标注触发警告的具体 UDP/TCP 对象，不能声称已经逐实例证明一定来自未使用的 TCP 路径；`rtc-methods.log` 也没有时间戳，不能从它单独重建每个 DELETE 与析构日志之间的精确间隔。若没有成功媒体链路和释放记录，不能仅按 `DTLS_HANG` 名称或上述机制忽略握手故障。

最后一条 `5008(RtcStun): no session` 出现在发布者 `9853zz83` 被管理踢出约 5 ms 后。`srs_app_rtc_server.cpp:374～430` 先按对端地址查会话；会话不存在时，DTLS/RTP/RTCP 也会进入使用 `ERROR_RTC_STUN` 的通用分支。此次日志字节以 `80 ef` 开头，符合 RTP v2 头部形状，不是 STUN 头部。因此错误枚举名不能用于推断“正在建立的 STUN 握手失败”；结合先移除会话、后收到单个数据报的顺序，应归为释放后的尾包。没有抓包，不能进一步断言其负载内容或完整在途路径。

RTC 审查只纳入 2026-10-22 本次的发布、观看、窗口、方法状态和服务日志。目录中 `integration.log` 的时间为 2026-10-18，记录的是先前失败；审查 JSON 明确列出该历史文件的排除原因，没有把旧失败当成本次结果，也没有删除它。
