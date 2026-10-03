# 第七阶段：聊天视频的 HTTP 点播

## 1. 点播、下载与播放的关系

点播的媒体已经存在，观看者可以自行选择开始时间和播放位置。HTTP 点播仍然在传输文件数据，只是不要求先取得整个文件才开始播放。

| 方式 | 数据读取 | 本地视频文件 | 起播条件 |
|---|---|---|---|
| 下载后播放 | Qt 网络模块下载；播放器随后读取本地文件 | 保存完整文件，校验长度及 SHA-256 | 下载和校验完成 |
| 已下载文件播放 | 确认聊天读取权限并校验缓存后，读取本地文件 | 复用已有副本 | 缓存校验完成 |
| 在线播放 | FFmpeg 通过 HTTP 按需读取远程文件 | 应用不创建视频文件，只保留有限内存缓冲 | 取得容器信息和足够的媒体数据 |

在线播放不等待全文件 SHA-256 校验。播放完整段视频仍需要传输相应的编码数据；暂停或提前关闭可以停止后续读取。内存缓冲、操作系统网络缓冲与磁盘中的下载缓存是不同概念。

```text
聊天消息中的 media_id
  ├─ 下载后播放：read 授权 → Qt HTTP GET → 文件校验 → OpenFile
  └─ 在线播放：read 授权 → HTTP 地址和请求头 → OpenOnline
                                                ↓
                   解封装 → 音视频解码 → 图像/PCM → 同步播放
```

## 2. 服务端：按字节区间读取文件

### 2.1 Range 与响应头

Range 的单位是文件字节，不是视频帧、编码包或秒。偏移从 0 开始，起点和终点均包含在区间内。

```http
GET /media HTTP/1.1
Authorization: Bearer <读取凭证>
Range: bytes=1000000-1999999
```

假设文件共有 10000000 字节，对应响应为：

```http
HTTP/1.1 206 Partial Content
Accept-Ranges: bytes
Content-Range: bytes 1000000-1999999/10000000
Content-Length: 1000000
```

| 字段 | 含义 |
|---|---|
| `Range` | 客户端请求的原文件字节范围 |
| `Accept-Ranges: bytes` | 服务端支持字节范围请求 |
| `Content-Range` | 本次返回的范围和原文件总长度 |
| `Content-Length` | 本次响应体长度，不一定是完整文件长度 |

`end - start + 1` 才是闭区间的长度。服务端允许请求终点超过文件末尾，但会截到实际末尾。

当前接口行为：

| 请求 | 处理 |
|---|---|
| 不带 Range | `200`，完整文件 |
| `bytes=A-B` | `206`，从 A 到 B |
| `bytes=A-` | `206`，从 A 到文件末尾 |
| `bytes=-N` | `206`，最后 N 字节；N 超过总长时取整个文件 |
| 起点已超过末尾，或后缀长度为 0 | `416`，`Content-Range: bytes */总长度`，空响应体 |
| 单段 bytes 语法错误、数字溢出、终点小于起点 | `400`，`invalid_range` |
| 多段范围或未知范围单位 | 忽略 Range，返回完整文件的 `200` |
| 带 `If-Range` | 当前未实现表示验证器，返回完整文件的 `200` |

`416` 不等于所有 Range 错误。多段请求也不应假装只满足其中一段；本阶段选择 HTTP 允许的忽略范围行为。

### 2.2 服务端调用链与 API

```text
accept：接收媒体 TCP 连接
→ serve：async_read_header 读取 HTTP 请求头
→ 检查凭证、聊天登录会话、会话成员与媒体可读状态
→ download：ParseRange 得到 offset、length
→ 工作线程打开文件，核对实际长度，seekg 定位
→ async_write_header 发送一次响应头
→ 循环读盘、async_write 发送，直到 length 个字节全部发完
```

| 代码/API | 作用 |
|---|---|
| `parser.get()[http::field::range]` | 从已经解析好的请求头取得 Range 文本 |
| `ParseRange()` / `ByteRange` | 将请求转换成偏移、响应长度、是否部分响应、是否可满足 |
| `std::from_chars()` | 解析无符号整数，并检查非法字符和溢出 |
| `std::ifstream::seekg()` | 移动文件读取位置，不需要从文件开头读到目标位置 |
| `asio::co_spawn(workers_, Work(...))` | 把同步文件操作放到工作线程，避免阻塞聊天事件循环 |
| `http::async_write_header()` | 发送状态码和响应头 |
| `asio::async_write()` | 将本块字节写入连接；返回不代表对方已经播放 |

每个响应复用一个 64 KiB 缓冲区。最后一次读取量是 `min(65536, remaining)`，否则可能越过承诺的区间终点。写完当前块才复用缓冲，不能在异步发送仍使用它时覆盖内容。

分块写入是同一个 HTTP 响应的多次写操作，不是每块返回一次 206，也不要求每块恰好对应一个编码包。TCP 自身还可以拆分或合并这些写入。

`response_started` 区分错误发生在响应头之前还是之后：头之前可以返回 JSON 错误；正文发送途中失败只能结束该响应，不能把另一份 HTTP 错误拼入视频字节。

### 2.3 权限和资源边界

在线读取与完整下载共用 `op=read`、GET 地址和 Bearer 凭证。客户端只有 `media_id` 不代表有读取权限；服务端继续检查登录用户与会话成员关系。

读取凭证有效期为五分钟，可在有效期内用于多次 Range 请求。每次 HTTP 请求都会重新检查凭证及关联的聊天登录连接；本实现不会在已获准响应的每个 64 KiB 块之间重新计时鉴权。

数据库连接在权限查询后释放，不随整个视频传输长期占用。HTTP 并发连接数已有上限，文件读写采用有界缓冲；小内存服务器不需要装入整份视频，也不需要解码或按帧率发送。

公网 Nginx 转发 Range 和 Authorization，关闭代理响应缓冲，使上游数据可以持续交付给播放器。媒体目录没有直接公开为静态目录。

## 3. 客户端：区分输入，共用播放

### 3.1 模块边界

| 模块 | 职责 |
|---|---|
| `media_chat.cpp` | 视频卡片、本地/在线按钮，绑定当前消息的授权回调 |
| `MediaTransfer` | 聊天控制请求、读取权限、下载缓存与摘要校验 |
| `MediaSource` | 描述输入类型、位置、展示名称、HTTP 头和到期时间 |
| `MediaWindow` | 播放状态、缓存、设备和时钟；需要凭证时发出信号 |
| `MediaDecoder` | 工作线程中打开输入、解封装、解码和转换 |

`MediaSource::Kind::LocalFile` 的 `location` 是路径；`Kind::Http` 的 `location` 是经业务模块检查的 URL。`headers` 只用于 HTTP，`expires_at_ms` 是凭证到期的 UTC 毫秒时间，不是媒体 PTS。

播放器不需要知道联系人、群组或 `media_id`。聊天模块也不负责逐帧解码。`MediaSource` 通过 Qt 元类型注册后，可以随排队信号复制到工作线程。

### 3.2 本地与在线的入口差异

```text
本地：fileReady(path) → MediaWindow::OpenFile(path)
                     → OpenSource(LocalFile) → OpenRequested

在线：视频卡片 → MediaWindow::OpenOnline(name)
             → OpenSource(Http) → AuthorizationRequested(session)
             → MediaTransfer::requestPlaybackSource → TCP op=read
             → 检查地址/授权头 → SetOnlineSource(session, source)
             → OpenRequested

共同：MediaDecoder::Open → OpenInput → avformat_find_stream_info
     → Opened → 设备协商 → RequestData → Read → BatchReady
     → AcceptBatch → TryStart → Tick
```

`OpenInput()` 集中处理读取差异：本地检查文件存在并打开路径；在线将 HTTP 参数交给 FFmpeg。其后的送包、取帧、像素转换和音画同步共用同一套实现。

这里集中的是“输入怎样打开”的差异。整个播放器还需要区分在线授权、预取时长、网络重缓冲、超时中断和凭证刷新，不能把所有在线处理理解为只改一个函数。

#### AVDictionary：打开输入时的参数表

```cpp
AVDictionary* options = nullptr;
av_dict_set(&options, "headers", source.headers.constData(), 0);
av_dict_set(&options, "rw_timeout", "10000000", 0);
```

`AVDictionary` 保存字符串键值对，例如 `rw_timeout → "10000000"`。它不保存视频字节，`av_dict_set()` 也不建立连接或发送 HTTP 请求，只是准备配置。

| `av_dict_set()` 参数 | 含义 |
|---|---|
| `&options` | 字典指针的地址；字典为空时函数可以创建字典并回写指针 |
| `"headers"` 等键 | FFmpeg 认识的选项名称 |
| 字符串值 | 对应配置；数字也以字符串传入，使用选项的组件再解析 |
| `0` | 默认标志；复制键和值，已有同名键时替换其值 |

函数返回非负值表示成功，负值表示错误。多个 `av_dict_set()` 向同一字典追加配置。`headers` 的值本身可以包含一行或多行 HTTP 请求头，例如 `Authorization: Bearer <凭证>\r\n`；字典内其他键不是自动发送给服务器的 HTTP 头。

#### avformat_open_input：把参数交给实际输入

```cpp
avformat_open_input(&raw, source.location.toUtf8().constData(),
    online_ ? av_find_input_format("mov") : nullptr, &options);
av_dict_free(&options);
input_.reset(raw);
```

| 调用部分 | 作用 |
|---|---|
| `avformat_alloc_context()` | 分配输入上下文，以便打开前配置中断回调 |
| `raw->interrupt_callback = {Interrupted, this}` | 指定函数和回调参数；FFmpeg 等待 I/O 时检查取消和超时 |
| 第一个参数 `&raw` | 输入上下文指针的地址；函数可初始化或更新它，失败时释放所传上下文并置空 |
| 第二个参数 `location` | 本地路径或 HTTP URL；底层按地址选择文件或 HTTP 协议处理 |
| 第三个参数 | 在线指定 `mov` 解封装器（也支持 MP4）；本地传空，让 FFmpeg 探测容器 |
| 第四个参数 `&options` | 将参数分配给输入上下文、协议和解封装器等对应组件 |
| `av_dict_free(&options)` | 释放返回的剩余字典条目，指针置空；已经应用的配置由对应组件持有 |
| `input_.reset(raw)` | 把输入上下文交给已有智能指针管理，最终用 `avformat_close_input()` 关闭 |

`avformat_open_input()` 打开输入并读取容器头信息，这时就可能访问网络；`avformat_find_stream_info()` 还可能继续读媒体数据以补充流参数。成功返回不表示已经下载整个视频，也不表示已经打开音视频解码器。

URL 决定“从哪里取字节”，`mov` 决定“如何解析这些字节所属的容器”。它们分别对应 HTTP 协议处理和 MP4 解封装，不能把 `mov` 当作视频解码器；视频编码的解码器仍由后面的 `codec_id` 选择。

打开函数会消耗识别到的字典选项，并通过 `options` 返回未识别的选项。拼错选项名可能留下未消费条目；字典被成功创建并不代表每个选项一定被应用。

#### AVIOContext：让解封装共用字节读取接口

`AVFormatContext::pb` 指向 `AVIOContext`，负责缓冲字节读取和定位。解封装器通过这一层取得字节，因此无需为本地文件和 HTTP 分别实现一套 MP4 解析逻辑。

```text
本地：文件协议读取磁盘 ──────────┐
                              ├→ AVIO 字节缓冲 → MP4 解封装 → AVPacket
在线：HTTP 协议取得响应正文 ────┘
```

在线模式是客户端中的 FFmpeg 发送 GET/Range 请求，服务器返回 HTTP 响应。HTTP 协议处理会解析状态码和响应头，并把响应正文中的文件字节交给上层；HTTP 头不会作为 MP4 内容送入解封装器。

读取主要由上层需求推动：`av_read_frame()` 请求编码包，解封装器需要更多字节时读取 AVIO 缓冲，缓冲不足再由 HTTP 层读连接、必要时发起下一段范围请求。一次 `av_read_frame()` 不等于一次 HTTP 请求：包可能已缓存，一个响应也可能包含许多编码包。

本项目的普通 HTTP 输入没有另开一个应用级“持续收包线程”；这些调用在现有解码工作线程执行，等待网络时可能阻塞该线程。GUI 仍由自己的事件循环处理操作，FFmpeg 的中断回调用于结束过时的等待。暂停上层读取后，已经发出的响应及操作系统网络缓冲中仍可能有有限在途数据。

`input_->pb->seekable & AVIO_SEEKABLE_NORMAL` 检查当前字节输入是否报告支持普通位置跳转；实际 seek 时仍可能遇到网络或授权错误。

| HTTP 输入选项 | 本项目取值和作用 |
|---|---|
| `headers` | 传入 Authorization，每个头以 `\r\n` 结束 |
| `protocol_whitelist` | 只允许 HTTP、HTTPS 及其 TCP/TLS 依赖 |
| `max_redirects=0` | 不跟随重定向，避免授权头被转发到其他地址 |
| `rw_timeout=10000000` | 网络读写等待上限，单位微秒 |
| `request_size=262144` | 单次 HTTP 请求最多取 256 KiB，不是媒体帧大小 |
| `short_seek_size=262144` | 小范围向前移动可以优先读过中间字节 |
| `multiple_requests=0` | 本阶段使用独立请求，匹配服务端每个响应后关闭连接 |
| `probesize` / `analyzeduration` | 限制探测预算；不代表整个播放器的内存或总下载上限 |

单次 HTTP 请求的 256 KiB、服务端写盘读取的 64 KiB、FFmpeg 的 `AVPacket` 是三种不同边界。客户端不使用 FFmpeg 的 `cache:` 协议，因此不会为了 seek 建立磁盘缓存文件。

### 3.3 时间 seek 怎样变成 Range

MP4 的 `moov` 保存轨道描述、时间及样本索引等元数据；`mdat` 主要保存编码媒体数据。播放器需要索引才能把播放时间与文件位置关联起来。

```text
用户拖到目标时间
→ StartSeek：冻结播放、清除旧输出、递增 session
→ MediaDecoder::Seek
→ av_seek_frame：根据 MP4 索引寻找目标之前的随机访问点
→ FFmpeg 的 HTTP 输入按需要请求对应字节位置
→ avcodec_flush_buffers：清理解码器内部旧状态
→ 顺序解码，丢弃目标之前的完整画面、裁剪音频
→ 缓冲足够后恢复播放，或只展示暂停预览
```

进度百分比不能直接乘文件长度作为 seek 偏移：音视频交错、可变码率、关键帧依赖和容器元数据都会影响位置。

`moov` 在尾部时，FFmpeg 可以先用范围读取取得尾部索引，再读取媒体数据；不必下载整个 `mdat`。`faststart` 把索引移到前面，有利于开始播放，但不等于重新编码或改变画质。重新封装会改变文件字节和摘要，不能直接覆盖已经发布并记录 SHA-256 的原文件。

### 3.4 在线缓冲和时钟

本地输入沿用约 150 ms 的预取；在线输入目标预取约 1 秒，起播或网络重缓冲目标约 500 ms。画面到达 32 MiB 预取高水位也可以满足视频起播条件，避免高分辨率画面为了凑时长无限堆积。另有画面总量 64 MiB/180 帧、PCM 积压两秒的保护上限。

这些数值属于本例的策略，不是 FFmpeg 或 Qt 的固定要求，也不是整个进程的内存上限。编码器参考帧、格式转换和网络栈也会使用内存。

```text
打开/seek → Buffering → 数据达到门槛 → Playing
Playing → 可用画面或音频即将耗尽 → Buffering
Buffering → 数据补足 → Playing
暂停时 seek → Buffering → 目标画面准备好 → Paused
```

`BufferUnderrun()` 保留队列和当前画面，暂停设备、冻结播放位置；它不重新 seek。后台在途读取完成后继续补充数据，`TryStart()` 判断可以继续播放时恢复时钟。网络暂时没有数据与真实文件 EOF 必须区分。

没有音频设备时，`AudioOutput::Open()` 返回 0：窗口通过 `AudioEnabledRequested(false)` 通知解码线程跳过音轨，以单调时钟播放视频，并显示无声播放提示。重新打开文件时重新探测设备；本阶段不做播放中的设备热切换。

### 3.5 取消与凭证刷新

Qt 排队槽必须等待工作线程回到事件循环。若 FFmpeg 正在等网络，仅排队发送“停止”不能立即打断它。

`RequestSession()` 只写一个原子代次，允许 GUI 直接调用；`AVFormatContext::interrupt_callback` 在网络操作中检查代次、线程退出请求和 15 秒操作期限。旧操作退出后，工作线程才能处理新的打开或 seek。普通 FFmpeg 对象仍只由工作线程访问。

代次还有第二个用途：`BatchReady`、`Opened`、授权回调等结果到达 GUI 后，再检查 session，防止旧画面、旧声音或旧凭证进入新一次播放。

```text
播放/恢复/seek 前发现凭证将到期
→ 记住播放位置和播放/暂停意图
→ RefreshAuthorization → AuthorizationRequested
→ 聊天模块重新执行 op=read
→ SetOnlineSource → 重新打开输入 → seek 回原位置
→ Buffering → 恢复原来的播放或暂停状态
```

凭证刷新由聊天模块执行，播放器不保存登录密码。网络超时、权限拒绝会给出错误；本阶段不无限重试。停止、切换输入和关闭窗口都使旧请求失效。
