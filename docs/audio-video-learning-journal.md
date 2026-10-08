# C++ 音视频学习记录

## 2026年10月1日

### tianmu_sama 已确认

- 学习目标是服务端接收、存储和发送视频，客户端发送、下载播放和在线流式播放。
- 前期辅助学习的小项目由 Codex 实现并讲解，tianmu_sama 通过阅读、运行和小修改理解机制。
- 接入两个聊天项目时，由 Codex 完成核心框架和外围接入，指导 tianmu_sama 编写内部关键代码，再共同检查验证。
- 学习计划只按阶段组织，不安排天数或学习时长；根据理解程度和阶段验收推进，保持最终目标。
- 每阶段的学习文档放在 D:\chat_server\ChatServer\docs\音视频学习。Codex 开讲前先写简要知识提纲及 API 名称和作用；阶段文档不放链接，知识直接写在文档内。后续问题先在对话中解释，等 tianmu_sama 要求总结时，再将相关问答精炼、去重并补入文档，不逐次自动追加问答。修改时保留 tianmu_sama 自己写的笔记。
- 学习文档以知识为主体：先说明基础概念、数据含义与概念之间的关系，再解释相关结构体和 API。不要写成进度日志、代码阅读路线或验收流程；构建、运行、测试记录留在示例 README 或本日志。使用必要的表格、内存示意和具体数值帮助理解，删掉重复说明与泛泛铺垫。
- tianmu_sama 反馈响应过慢、实现验证耗时过多，而基础概念讲解不足。窄问题直接解释已有代码，避免无关检索和操作；开讲像素转换前先解释编码格式、像素格式、帧类型、平面、行跨度和色彩属性，不假设这些知识已经掌握。
- 服务端运行于内存较小的 Linux 服务器，希望在 Windows 本机环境完成构建准备。

### 已核实事实

- Qt 5.15.2 和 6.11.0 安装目录均在 F:\Qt；项目有 Qt 5 构建记录。实际开课使用的 Kit 尚待确认。
- 本机存在 ChatUbuntu WSL2 和 F:\linux\_environment\build-chat.sh，可供后续沿用本机 Linux 构建流程；本次只检查安装和脚本。
- FFmpeg 命令行 8.1.1 可运行。另已准备开发库 `D:\chat\_deps\ffmpeg-8.1.1-full_build-shared`，包含 include/lib/bin，压缩包 SHA256 与发布者记录一致；独立示例已验证链接和运行。
- D:\test_video.mp4：5 秒、40 帧、8 fps、1820×1030、H.264 High 4:4:4 Predictive、yuv444p、无音轨、尾部 moov。完整解码与逐帧计数成功。
- 服务端下载已按 64 KiB 分块读取并等待发送，尚无 Range 支持；已有文件单文件限制 100 MiB。
- 读取票据有效期五分钟，可在期限内重复读取，且关联登录会话；网络播放阶段需处理过期后重新获取授权。
- 第一阶段补充探测：前五个包的 PTS 秒数依次为 0、0.5、0.25、0.125、0.375，DTS 秒数为 -0.25、-0.125、0、0.125、0.25；包 duration 为 2048，以 1/16384 时间基换算为 0.125 秒。解码帧显示顺序前五帧为 I/B/B/B/P，PTS 为 0、0.125、0.25、0.375、0.5 秒，可用于讲解重排序。has_b_frames 不代表文件的 B 帧总数。

### 建议与待确认项

- 建议 Qt + FFmpeg 主线，HTTP 点播为第一轮闭环；相关客户端选项与“实时发送”含义的问题已发出，尚未收到明确选项答复。不要把建议写成已确认决定。
- 第一阶段示例位于 `lessons/media_info`，使用 Qt 5 附带的 MinGW 8.1.0 x64、CMake 3.30.5、Ninja、FFmpeg 8.1.1；实际 Qt 客户端集成的 Kit 仍待确认。
- 音频与同步阶段需要额外样本；网络阶段需要较长样本与限速实验。
- 服务器当前架构、系统版本、可用内存和运行库在部署阶段核实，不能仅依赖历史验证记录。

### 进度与下次入口

阶段 1 的理论已讲解。tianmu_sama 表示大致理解流、包、帧以及帧类型和两套时间戳的关系，希望结合有注释的实际代码建立操作感。教学主体是 `lessons/media_info/main.cpp`，`Inspect()` 保留探测和读包，`--decode <output.ppm>` 模式接入视频解码、EOF 排空和首帧导出。

本阶段笔记位于 [第一阶段 媒体信息查看器](D:/chat_server/ChatServer/docs/音视频学习/01-媒体信息查看器.md)。已按 tianmu_sama 要求整理两轮问答，补入 I/P/B 的预测与残差、解码和显示顺序、时间戳字段及零点、负 DTS 与播放器调度，并移除阶段文档全部链接。

程序以 Debug/x64 构建通过，本次增量编译 7.97 秒，产物 651187 字节，2026-10-01 16:22:07，PE x64。10 项读包测试与 11 项解码测试通过；解码帧字段逐项对照 ffprobe，首帧 RGB 与 FFmpeg 逐字节一致。构建与运行命令记录在 `lessons/media_info/README.md`，阶段文档已补入代码阅读顺序，不包含链接。

本次排查记录：MinGW 的 `cc1plus.exe` 依赖 bin 目录中的 `libwinpthread-1.dll`，命令行构建需要在 CMD 子进程 PATH 中加入编译器 bin；配置命令与构建命令均需带该设置。MinGW 8.1 默认 Windows API 目标过低，`WC_ERR_INVALID_CHARS` 受 WINVER 条件限制；示例 CMake 明确 WINVER 和 _WIN32_WINNT 为 Windows 10。无效文本若命名为 `.dat`，FFmpeg 8.1.1 会低置信度选择 luodat，之后在探测阶段报 EOF；`.bin` 在打开阶段报无效数据，测试分别捕获并检查预期日志。

tianmu_sama 已阅读核心代码，并提问自定义删除器、流信息探测、结构体成员和时间单位、解封装器名称及包的提取与归属。本轮明确要求将重点总结入文档，并表示第一阶段差不多，可以进入下一阶段；已补入第一阶段笔记，不把推进阶段等同于所有细节均已完全掌握。

讲解反馈：先前“解封装器已经知道包的归属”的表达造成困惑。后续解释应明确区分 `av_read_frame()` 内部完成解析、提取和填写字段，以及函数返回后调用方按 `stream_index` 使用结果这两个时刻。避免用抽象的“分发”代替对具体调用过程的说明。`AVInputFormat` 对应解封装器而非解码器，名称列表属于解封装器的公共描述，选择仍与文件容器有关。

下一入口为 `docs\音视频学习\02-视频帧导出器.md` 与代码中的 `VideoDecoder`。tianmu_sama 明确要求在第一阶段程序上扩展并观察实际执行；已实现带中文注释的构造、`Send()`、`Receive()`、`SaveFirstFrame()`。接下来先带读解码实例初始化和送包/取帧，再解释 AVFrame 平面、行跨度与 RGB 转换。教学程序已完成，不等于 tianmu_sama 已学完第二阶段。

tianmu_sama 随后逐项询问了 context_、frame_、FramePtr、解码初始化 API、Receive、像素格式名称、RGB/YUV、AVFrame 字段、色彩属性和 SwsContext，并要求重写阶段文档。第二阶段文档已按知识主题重组，删除进度、运行命令、验收和阅读路线，集中解释概念关系、内存表示、对象职责、API 状态和资源所有权。后续文档沿用该知识笔记形式，避免把用户的“大致理解”当作所有知识均已掌握。

实测样本 40 帧，其中输入 EOF 后排空得到 2 帧；单线程解码先送包 0、1、2，才输出 PTS 0 的首帧。首帧 1820×1030、yuv444p，三个平面行跨度均为 1856。代码只将选中第一条视频流送到一个持续存在的 AVCodecContext，其他包只展示；两条同编码视频流也不能共用一个工作实例。强调输出延迟不等于一张图必然拆在多个包内，send/receive 分离也不要求两个线程。

像素转换采用已安装 FFmpeg 8.1.1 的 `sws_alloc_context()` / `sws_scale_frame()` / `sws_free_context()`，通过帧传递颜色参数。完整范围 H.264 测试输出 yuvj444p，会有 swscale 弃用提示；库头文件及官方实现确认是格式提示，测试精确捕获提示并验证像素一致，未屏蔽日志。MinGW 8.1 的 fstream 不支持直接传宽字符文件名，本示例通过 `_wopen` 排他创建输出，保留中文路径且防止覆盖输入。

样本及演示输出：原视频 SHA256 仍为 BBCC49A308156A62F0C93DFFE16D72EF3C662AFF791E75EA5B1E60C1B59AF116；`lessons/media_info/build/first-frame.ppm`、用于预览的 `first-frame.png`、完整 `decode-sample.txt` 已生成。PNG 由 FFmpeg 从 PPM 转换，教学程序自身导出 PPM。已查看预览，方向和内容正常。

### 第三阶段：Qt 无声播放器

tianmu_sama 已授权实现简单的第三阶段项目，接受本地打开、按 PTS 播放、暂停恢复和解码不阻塞界面的范围，要求知识笔记关注本阶段的新知识。项目在 `lessons/video_player`，独立于前两阶段示例。`VideoDecoder` QObject 归属后台 QThread，每次请求返回一帧独立 QImage；`PlayerWindow` 使用 QElapsedTimer 与 QTimer 调度，`VideoView` 在 paintEvent 中保持比例绘制。最多一张待显示帧，未引入复杂队列、网络、音频或 OpenGL。

第三阶段知识文档为 `docs/音视频学习/03-Qt无声视频播放器.md`，讲播放时钟、PTS 归零、暂停时间累计、定时器误差、重绘与显示的区别、QImage 外部内存与 copy、线程归属、QueuedConnection、有限缓存、会话编号及线程关闭。文档没有链接或构建验收流程，操作记录在示例 README。不要把“程序已实现”当作用户已学完这些概念。

Debug/x64 的 silent_player 与 playback_test 完整构建通过，成功构建耗时 19.80 秒；真实媒体的 6 项功能测试全部通过（加初始化和清理共 8 passed），并在 Windows 平台额外通过暂停/恢复/重绘检查。样本 40 帧、约 5 秒，覆盖变帧率、暂停冻结、首帧像素、中文路径、切换文件、非零首 PTS、重播、保持比例、错误恢复、线程关闭。运行说明、准确命令、产物大小和时间保存在 README。

构建经验：本机 Qt 5 MinGW SDK 只提供 Release 插件，不能从应用 Debug 配置猜测 qwindowsd.dll 路径。CMake 使用 Qt5::QWindowsIntegrationPlugin 和 Qt5::QOffscreenIntegrationPlugin 的 TARGET_FILE 获取真实文件。初次失败只发生在插件复制，诊断后修改 CMake 并完整构建通过。Qt Test 在该环境默认输出没有被子进程捕获；显式 -o 文件,txt 才获得可检查的日志，测试脚本必须核对 Totals 和意外警告，不能仅检查退出码。offscreen 绘制需要 QT_QPA_FONTDIR 指向 Windows Fonts，否则预览文字为空；真实 Windows 绘制已验证正常。

第三阶段队列调研：tianmu_sama 要求根据真实播放器源码解释策略，不要仅附和其“单帧请求设计差”的推断。Qt Multimedia 6.8 FFmpeg 后端 StreamDecoder 用未处理帧数量限制解码推进，视频阈值为 3；Renderer 持有 m_frames 队列，按 PTS 调度，处理后回传 frameProcessed，减少计数并推进解码。阈值不是全进程最多 3 张图的绝对内存上限。Demuxer 按每条活动流的编码包缓存时长约 4 秒或字节数 32 MiB 限制读包，不能误称 4 秒 RGB 帧队列。Haruna 使用 Qt/QML 与 libmpv；mpv 官方 stable 手册中额外解码线程/帧队列 vd-queue-enable 默认关闭，支持帧数、字节、时长限制，这不代表整个 mpv 无缓存。当前教学播放器也是显示当前帧时预取下一帧，只是最多一帧待显示，缓解耗时波动能力有限。用户本轮只要求研究解释，尚未授权改队列；问答暂不追加阶段知识文档。
文档整理与阶段约定：tianmu_sama 明确同意在第五阶段完成后扩展有界帧队列，已同步学习方案，第三阶段保留单帧预取。第一阶段笔记参照第二阶段按知识主题重组，保留结构体及成员、I/P/B 预测、时间戳和负 DTS、解封装归属及资源所有权，去掉命令、验收和进度记录。第三阶段增加 VideoDecoder 的对象关系、Open 初始化链、Read 先 receive 再按 EAGAIN 读包/送包的分支循环、EOF 排空、像素转换及 Qt 绘制衔接。数据流箭头与实际调用顺序明确区分，两份阶段文档均不添加链接；本轮只修改文档。
2026-10-02：应 tianmu_sama 要求，将 QObject 对象实例的线程归属、QThread::start 默认经 run/exec 启动工作线程事件循环、构造函数仍在 GUI 线程继续执行，以及普通直接调用与 QueuedConnection 的区别，精炼补入第三阶段第 6 节。讲解保持对象实例与类、QThread 管理对象与其工作线程的区分。
### 第四阶段：音频解码与 PCM 示例及知识笔记

tianmu_sama 要求先做简单音频解码程序，重点讲清基础知识与 API，允许给现有视频副本加测试音轨。采用独立控制台示例 lessons/audio_decode，将第一条音轨解码并转成 48 kHz、双声道、S16 小端交错 PCM；Qt Core 仅处理命令行和 Unicode 文件路径。本次不把完整音频设备播放或第五阶段音画同步提前加入。原视频 SHA256 未变，副本 samples/test_video_with_audio.mp4 已加入 44.1 kHz 双声道 AAC，左 440 Hz、右 880 Hz，幅度各 0.1，视频流复制。

- [x] 编写真实媒体测试并确认红灯：程序尚未实现/构建，PCM 字节一致性测试失败。
- [x] 实现并构建解码与重采样示例，核对实际样本和参考 PCM。
- [x] 编写知识文档、API 调用关系和运行说明，记录验证结果。

独立示例 main.cpp 实现第一条音轨选择、send/receive、按首帧实际格式建立 SwrContext、输出容量估算、S16 交错写文件，以及解码器和重采样器分别排空。声道布局缺少位置标记时 describe 可能返回带空格的 “1 channels”；初次测试错误源于日志解析按空格拆字段，按下一字段名边界提取后解决。九项真实文件测试通过，成功结果与 FFmpeg CLI 逐字节一致，首帧元数据与 ffprobe 对照；所有预期失败检查 stderr，不忽略诊断。测试变量命名统一为 pcm_bytes。

样本 AAC 解码得到 216 帧、每声道 221184 个采样；目标每声道 240745 个采样，962980 字节、5.015521 秒，最后从 SwrContext 取出 17 个采样。首包带 skip_samples=1024，末包 duration=340 但末解码帧 nb_samples=1024；本例不按容器五秒裁切尾部。swr_get_delay 在空输入返回 0 后仍可为 17，排空结束依据 swr_convert 返回 0。第一帧 FLTP 的每平面有效数据 4096 字节，而 linesize[0] 实测 8192，文档据此解释有效长度与容量。

产物 audio_decode.exe / Debug / x64：Full build passed。首次构建 13.13 秒，格式化和显式头文件后的最终增量构建 7.52 秒，929005 字节，2026-10-02 14:15:14（Asia/Shanghai），PE Machine 0x8664；准确命令存于示例 README。已生成 build/decoded.pcm、decoded.wav、decode-sample.txt。MP4 副本的视频编码数据哈希与原视频一致，WAV 采样与导出 PCM 一致；未声称真实设备播放验证。第四阶段知识文档为 docs/音视频学习/04-音频解码与PCM.md，无链接和运行验收流程。设备播放与用户对知识的掌握仍待后续学习，不能因为示例已完成就跳到第五阶段。

2026-10-02 验证方式调整：tianmu_sama 明确反馈阶段生成耗时过长，要求今后这类音视频学习大阶段示例不采用 TDD，不写大量测试，只做必要验证。此要求覆盖此前针对这些学习示例的先写失败测试约定；后续直接实现，按目标完成必要编译、代表样本运行和关键结果核对，具体风险才增加异常验证，不机械沿用九项或更多测试矩阵。重点保留基础知识、API 关系和中文注释。学习方案已同步。
2026-10-02 API 讲解反馈：tianmu_sama 指出 Convert 的输出归属、swr_convert 核心参数及声道解释仍不清楚。讲解核心 API 时需同时说明每个参数读写哪块内存、计数单位、返回值和元数据来源。已补齐第四阶段 API 小节：pcm_ 是实际输出存储，output_data 只是别名，&output_data 提供单平面指针入口，output_.write 读同一内存写文件；输入样本格式和声道顺序由 SwrContext 中的预配置决定，extended_data 自身不带这些标签。

2026-10-02 第四阶段问答整理与第五阶段范围讨论：应 tianmu_sama 要求，将 data/extended_data 的指针表关系、平面不一定是连续二维数组、同帧各声道 nb_samples 相同、三声道交错、send 的 EAGAIN 与内部缓存、外部队列持有帧引用、PrepareResampler 的准备职责，以及 S16P 输出缓冲和裸 PCM 文件排列补入第四阶段知识文档；知识文档继续不放链接。

本轮明确不实施第五阶段，只说明在现有无声播放器中加入音频解码与设备输出、音频参考时钟、点击/拖动 seek 的范围及新知识。音量、静音、停止/重播、全屏和快捷键为建议，尚未作为独立新增功能全部获得实施授权。有界多帧预取扩展仍安排在第五阶段之后；必要的音频设备缓冲属于正常播放基础，不等同于该性能扩展。

用户要求使用网上现成的双声道视频，不自行制作。本轮下载 https://media.w3.org/2010/05/sintel/trailer.mp4 至 lessons/video_player/samples/sintel_trailer.mp4，未改音视频数据；ffprobe 确认 4,372,373 字节、52.208333 秒、H.264 854×480/24 fps、AAC 48000 Hz/2 声道/stereo。来源及元数据写入同目录 README。只确认文件和媒体参数，未声称播放器已完成音频或真实设备播放验证。

### 第五阶段：实施任务

2026-10-02：tianmu_sama 授权开始第五阶段，要求模块职责明确，每个头文件函数和成员都有基础中文说明，源文件关键逻辑补充中文注释；阶段文档重点讲清 Qt 音频 API、参数、对象关系及调用链。沿用本阶段示例不做 TDD、只做必要验证的约定。

- [x] 在现有 video_player 工程中完成有声播放入口、解码/音频输出/播放调度模块和进度跳转、音量、全屏等控制。
- [x] 编写第五阶段知识文档与运行说明，核对头文件函数和成员注释。
- [x] 完成目标构建和真实样本必要验证，记录结果与边界。

实现入口为 lessons/video_player/build/media_player.exe。同一工程保留第三阶段 silent_player，抽取原有 VideoView 供两者复用，原绘制实现与注释保留。第五阶段用 MediaDecoder 管理一次解封装、两种解码及转换，AudioOutput 管理 Qt 音频设备与 PCM，MediaWindow 管理播放状态和调度；各头文件函数、信号和成员已写中文说明。正常准备约 150ms 数据，声画交错需要基础缓存；64 MiB/180 张图像和两秒 PCM 为硬上限，后续队列专题继续研究可配置容量、分轨包缓存与背压，不把基础缓冲当成性能专题已完成。

重要实现依据：已读本机 Qt 5.15.2 源码 qwindowsaudiooutput.cpp，processedUSecs 基于 write 中累加的 totalTimeValue，属于提交量；bytesFree 基于归还的设备周期。采用完整周期写入并用提交字节减实际缓冲占用估计消费位置，精度约一周期，不宣称物理声画零偏差。公共时间轴保留音轨起始偏移，音频比视频先结束后接续单调时钟。seek 清 GUI、设备、解码器、重采样器并换会话编号，音频按采样裁剪，暂停定位只预览不写音频。

必要验证中的两项调查：初次 Qt 音频格式检查失败，WinMM waveOutGetNumDevs=0、waveOutOpen query 返回 MMSYSERR_BADDEVICEID；并非格式转换代码问题。tianmu_sama 说明此前没接耳机、现已接好；再次枚举为两个输出设备，真实播放验证通过。另一个检查最初假定首解码音频时间为零，ffprobe 确认 Sintel 首解码帧为 0.021333 秒（包起点为零）；检查按实际参考结果修正，播放器保持该偏移。问答中若讲起始零点，应区分包时间和实际可输出音频帧。

最终 media_player/media_smoke Debug x64：Full build passed，无编译警告；播放器最终源码构建 8.64 秒，随后验证代码增量构建 5.44 秒。产物分别为 5250205 字节（16:26:58）、6090456 字节（16:32:16），PE 0x8664。三项真实功能验证及初始化/清理共 5 passed、0 failed、0 skipped，13.411 秒；覆盖双声道解码、非关键帧位置 seek、真实音频设备推进、暂停恢复、暂停 seek 预览、滑槽点击、尾部结束、重播、停止及原无音轨样本。没有执行大测试矩阵或 TDD。准确命令及限制在 MEDIA_PLAYER.md，知识文档为 docs/音视频学习/05-音画同步与进度跳转.md（无链接）。本阶段实现完成不等于用户已掌握；下一次从该文档中的对象关系和音频 API 开始带读。

2026-10-02 文件头约定：tianmu_sama 明确要求本项目不使用 ABOUTME、copyright、author、date 这组文件头。本轮已清理 ChatServer 自有源码、教学示例、构建文件和测试脚本中的对应标记，保留中文知识注释和其余内容；后续新文件不再自动添加这些文件头。该明确项目偏好优先于 cpp-utf8-bom 技能中的版权模板要求，UTF-8 BOM 编码约定仍保留。

2026-10-02 主时钟知识补充：tianmu_sama 要求将音频 PTS、PCM 采样时间、静音与缺失数据的区别写入第五阶段，用来解释音频主时钟的选择。已精炼补入第 4、5 节：裸 PCM 不带 PTS，设备按采样率连续消费；主时钟选择取决于连续输出及同步调整代价，不取决于轨道时长。明确共同零点与主时钟的区别、音轨先结束后的时钟接续，以及本例尚未完整处理音轨中途任意时间戳跳变。保留无链接的知识文档风格。

2026-10-02 第五阶段问答整理：按 tianmu_sama 要求，将学习笔记中的状态机、缓冲门槛、seek 调用链和分层清理、音频连续消费与断供、帧时间字段、RequestData/Tick 协作整合到各自章节，避免按问答顺序追加。文档不添加链接，不记录实施流水账；区分有效 PCM 末尾、设备已消费位置与整体播放结束，明确 Seeked 不等于目标帧就绪、FramePresented 不驱动绘制。

本轮静态核对确认两项实现边界，已如实写入阶段笔记：Playing 暂时缺数据不会自动转 Buffering；若在 Buffering 且 play_after_buffering_ 为 true 时按住进度条，sliderPressed 不会关闭自动播放意图，缓冲完成可能在松手前进入 Playing。后者与“拖动期间冻结播放”的一般预期不完全一致，留待专门处理交互边界；本轮用户请求是整理知识文档，未修改播放器行为。

2026-10-02 阶段六前连接预检：按 docs/BUILD_DEPLOY_HANDOFF.md 连接 lth@39.105.18.142:22。非交互认证未通过，用户随后提供凭据；通过 SSH 交互密码成功登录，sudo mysql 只读查询成功。远端 Ubuntu 24.04.4 LTS / x86_64；chatserver.service 为 active/running、MainPID 65530、NRestarts 0，MySQL 和 Redis 均 active；6000 监听 0.0.0.0，6002 监听 127.0.0.1。内存总量 1612 MiB、当时可用 783 MiB，swap 4095 MiB、未使用；MySQL 8.0.46。未验证应用数据库账号连接，未构建、部署或更改服务；退出本次 SSH 会话。登录时拒绝了 oh-my-zsh 更新提示。凭据不写入笔记、脚本或仓库。

### 第六阶段：Qt 聊天 MP4 收发与本地播放

tianmu_sama 已授权直接实现 Qt 客户端接入，先支持 MP4 上传、收到后点击下载并播放，封面等细节不做。第五阶段的队列扩展推迟到服务端流媒体学习完成之后。本轮继续按学习项目直接实现、必要验证的约定推进；服务端如需改变，只在本机 WSL 编译 Linux 产物后部署。

- [x] 核对服务端现有协议、Qt 5 工程和播放器接口。现有 msgid 26/27/28 已支持 begin_file、publish、read、history 和实时通知，预计不需要修改服务端。
- [x] 接入 Qt 文件传输、消息展示和第五阶段播放器，保持有限缓冲与明确失败反馈。
- [x] 构建 Qt 客户端，完成实际服务收发、无声播放和历史恢复验证，补充第六阶段知识笔记及运行说明。
- [ ] 当前系统未枚举到音频输出设备；连接耳机后补充双声道样本的实际播放验证。

第六阶段实现：Qt 新增 MediaTransfer（TCP 控制请求、QFile 流式 PUT、QSaveFile 分块 GET、长度/SHA-256 检查、取消和超时），media_chat.cpp 负责消息卡片、现代媒体历史和播放器入口。第五阶段通过 media_playback.pri 直接编入 Qt 客户端；媒体消息在历史加载完成后追加去重。Qt 默认聊天入口改为 127.0.0.1:16000，HTTP 描述使用 127.0.0.1:16001，两者均经 SSH 隧道。阶段六原范围限定 MP4 完整下载后播放，没有新增网络点播或封面。

验证：ChatClient / Debug / MinGW x64 Full build passed，最终增量 8.77 秒；产物 E:\chat_server_qt\ChatClient\build\Desktop_Qt_5_15_2_MinGW_64_bit-Debug\debug\ChatClient.exe，18,813,912 字节，PE 0x8664。准确构建命令、依赖及隧道用法写入客户端 README。现有头像未使用 event、LoginWindow 成员初始化顺序、QTcpSocket 旧 error 信号产生编译警告；均为既有代码，未扩大修改范围。windeployqt 初次使用 --release 时按 PE 调试标记过滤掉了本 SDK 插件，使用匹配此 Debug 产物的 --debug 后部署成功；FFmpeg DLL 已复制到输出目录。

必要联调未使用模拟服务：tests/video_chat_smoke.pro 使用两个实际账号调用现有服务，上传、推送、下载校验通过。双声道 Sintel 文件成功传输，但 Qt 当前枚举音频输出设备为 0，播放器明确报告设备不可用；已询问 tianmu_sama 当前耳机情况，未声称已听到声音。原 D:\test_video.mp4 全流程 4.18 秒通过：MP4 收发、SHA-256、画面与时钟、暂停 seek、历史恢复且不重复、文字消息。截图和日志位于客户端 build/video-chat-smoke，已查看画面。首次测试对文件选择框的单次定时确认未生效，测试停在 QFileDialog::exec；通过调试栈定位并取消测试对话框后，联调直接向真实传输模块传入样本路径，不再自动操作文件选择框。没有修改生产文件选择逻辑来迁就测试。

本次专用账号：7/8（av6_d8b838ce_a/b，仅界面尝试），9/10（av6_5075d694_a/b，Sintel 传输），11/12（av6_0c934dc3_a/b，无声全流程）。这些账号与测试发布文件保留在服务端；不向既有联系人发送测试消息。未把登录口令或临时 Bearer 凭证记录到仓库。服务端源码及部署不需改变。
2026-10-03 连接失败排查：服务器 chatserver.service 保持 active/running，PID 65530、NRestarts 0，自 2026-10-01 06:27:13 CST 运行；6000/6002 正常监听，MySQL/Redis active。根因是 Windows 本地 16000/16001 没有监听，上一轮临时 SSH 隧道已退出，而 Qt 默认依赖这两个转发端口。已重建双端口 SSH 隧道，通过匿名媒体控制请求得到 msgid 27 unauthorized、HTTP /media 得到 unauthorized，确认两条转发均到达真实服务。匿名拒绝符合预期，没有执行登录或业务写操作。客户端连接失败后没有自动重连，需重新打开客户端。隧道是本轮临时连接，不是持久系统服务，后续使用仍需保持 README 所述 SSH 命令运行。
2026-10-03 连接方式解释纠正：tianmu_sama 追问此前已经能通过 HTTP 发文件。核对已有 Electron package.json、IMAGE_FEATURE.md、FILE_FEATURE.md、LOCAL_STORAGE.md：原图片/文件方案已使用 npm run tunnel，将本地 16000/16001 转发到远端 6000/6002；并非第六阶段首次引入隧道。第六阶段改变的是 Qt 的默认聊天地址，复用了既有 Electron/服务端的接入配置。应区分“应用协议 HTTP”“承载请求的 SSH 转发”“客户端与服务端分别运行”三个层次，不能把旧文件方案说成已公开 HTTP 直连，也不能把转发解释成客户端服务端不分开。
### 移除客户端隧道依赖

2026-10-03：tianmu_sama 已授权改成直连，并明确选择“公网 IP + HTTP，用于当前学习联调”，已知文件和授权凭证不加密。优先复用现有 Nginx 反向代理，文件服务继续只监听服务器回环地址，不为此改变 C++ 服务端。客户端保留文件授权和完整性校验，只对明确配置的媒体 HTTP 地址允许公网访问。

- [x] 配置并核实公网文件入口及返回地址，保留配置备份。
- [x] 修改 Qt 客户端默认连接与允许地址，同步原 Electron 文件客户端默认地址。
- [x] 验证无需隧道的实际请求及收发，更新运行说明。
直连进展：确认公网 TCP 7000 经既有 Nginx stream 可正常返回 msgid 27，Qt/Electron 默认恢复到该入口。Nginx HTTP 配置已安装并 reload，`nginx -t` 成功；服务器本机带 Host 39.105.18.142 请求 /media 得到预期 unauthorized。文件服务仍监听 127.0.0.1:6002，新增配置关闭上传和下载代理缓冲，100 MiB 限制。配置备份 /home/lth/chat_server/backups/direct-http-1790993472233。systemd 的 media-url.conf 已放置并 daemon-reload，但尚未重启 chatserver，运行中进程仍返回旧的回环 URL；等待公网 80 开通后再重启切换并进行完整无隧道验证。服务器 UFW inactive，iptables INPUT ACCEPT，本机 80 正常但外网 80 多次连接超时，已请用户在阿里云安全组放行，答复尚待收到。

用户已明确授权关闭 Qt 客户端、编译并实际验证。ChatClient / Debug / MinGW x64 Full build passed，14.88 秒，产物 18,815,102 字节、2026-10-03 10:11:49 CST、PE 0x8664。命令在原构建目录、原受控 PATH 下执行 cmd.exe /d /c F:\Qt\Tools\mingw810_64\bin\mingw32-make.exe -j4 debug，输出 direct-build.log；既有头像未使用 event 警告仍在。video_chat_smoke 同配置构建通过 9.18 秒，已将测试入口改为公网 7000，尚未运行本次公网文件验证，避免在已知 80 不通时创建多余测试账号。
2026-10-03 10:20 CST 直连完成：tianmu_sama 在阿里云安全组放行 HTTP 80 后，公网 /media 可达并正常拒绝匿名请求。按已授权方案重启 chatserver，运行时环境实际为 CHAT_MEDIA_URL=http://39.105.18.142；服务 active/running、PID 74471、NRestarts 0。关闭本会话建立的 SSH 隧道（原 Windows PID 23916），确认 16000/16001 均无监听，再运行已构建的真实 Qt 联调程序：公网 7000 登录与控制、公网 80 视频上传/下载、SHA-256、画面播放、暂停 seek、历史恢复、文字消息全部通过，6.03 秒。Nginx 日志 PUT /media 200、GET /media 200/561704 字节对应原 D:\test_video.mp4。日志 E:\chat_server_qt\ChatClient\build\video-chat-smoke\run-direct.log。新增专用测试账号 13/14（av6_58844103_a/b）及测试文件保留服务端，不向真实联系人发送消息。未重新编译服务端或执行数据库迁移；更新 Qt README、Electron 操作说明及 BUILD_DEPLOY_HANDOFF 当前入口说明。双声道实际出声仍保留上一阶段的设备待验事项，本次仅验证原无音轨样本，不把网络任务扩大到音频设备排查。

## 2026年10月3日：学习统筹与分阶段交接

tianmu_sama 希望本会话统筹后续音视频和流媒体学习，按需生成每阶段独立任务文档，再自行下发其他会话执行。本轮确认已走完前六阶段，暂不生成或实施第七阶段。统筹约定保存在项目根目录 context_transfer_report.md；后续接手先读该记录，再结合本日志和总计划。

每阶段必须交付 docs/音视频学习 下的知识文档：不放链接，以独立可理解的知识讲解为主，讲清概念、函数、结构体、字段、数据流及调用关系，避免依赖现成代码。构建部署遵循 docs/BUILD_DEPLOY_HANDOFF.md，在本机 WSL 编译 Linux 产物后发布。tianmu_sama 明确要求账号和密码写入本地交接记录，说明该记录不会上传；完整连接参数与 SSH/sudo/MySQL 凭据已保存到项目根目录 context_transfer_report.md，执行会话直接读取，不必重复索取。

tianmu_sama 已确认：以教会用户为目标，后续阶段由执行会话完整实现、做必要验证并编写知识文档，不预留核心代码让用户补写；不采用 TDD，不铺开大量测试；代码不添加 about_me / ABOUTME 或 copyright 文件头，保留有教学价值的中文注释。已同步总计划和统筹记录中的分工及验证约定。阶段六双声道实际出声的待验证记录继续保留，不与第五阶段独立播放器已通过的设备验证混同。

### 2026-10-03 视频传输界面与本地缓存修正

tianmu_sama 反馈传输完成后弹窗残留、视频卡片和输入区布局松散、重复点击每次重新下载。根因核对本机 Qt 5.15.2 qprogressdialog.cpp：reset 仅在 autoClose 或 forceHide 时隐藏，当前代码关闭 autoClose 后只 reset。下载每次使用 QTemporaryDir 下的随机文件名，没有命中检查，退出还会删除。

- [x] 修复弹窗生命周期，收紧消息卡片与输入区布局。
- [x] 持久缓存已校验视频，授权后分块检查缓存，缺失或损坏重新下载。
- [x] 增量构建并用真实服务验证弹窗关闭、缓存复用、损坏恢复和界面截图，更新说明。

完成：QProgressDialog 启用 autoClose，autoReset 保持关闭，由业务 finished 统一 reset；成功、失败、取消共用结束逻辑。文件卡片限宽 380，发送靠右、接收靠左，文件名中间省略并保留完整提示；输入区高 150，操作按钮高 36，视频入口与发送按钮分置底栏两侧，恢复按需滚动条。

缓存使用 QStandardPaths::GenericCacheLocation/ChatClient/videos/<sha256>.mp4，本机为 C:/Users/lds/AppData/Local/cache/ChatClient/videos。每次先 read 授权，再按 64 KiB 校验已存在副本；有效时直接 fileReady，损坏/缺失时使用原 HTTP 下载及 QSaveFile 提交。按内容摘要命名，不依赖进程内索引，不复制上传源或改动服务端。按钮在缓存存在时显示“播放”，提示实际路径；内容仍在播放前校验。

沿用此前关闭客户端、编译和验证授权，向运行中的 PID 4708 正常发送窗口关闭请求，进程正常退出，未强杀。两个已有 qmake 构建目录分别执行 cmd.exe /d /c 'set "PATH=F:\Qt\Tools\mingw810_64\bin;F:\Qt\5.15.2\mingw81_64\bin;C:\Windows\System32;C:\Windows" && F:\Qt\Tools\mingw810_64\bin\mingw32-make.exe -j4 debug > cache-layout-build.log 2>&1'。ChatClient / Debug / MinGW x64 Full build passed，7.48 秒，产物 debug/ChatClient.exe 18,910,845 字节，2026-10-03 10:43:44，PE 0x8664；video_chat_smoke 同配置 7.64 秒，18,012,095 字节，10:44:05，PE 0x8664。本次编译无警告。

真实服务联调 5.53 秒通过：上传/实时推送/下载校验/画面播放/暂停定位/历史/文字；完成与取消后弹窗隐藏；重复点击路径和修改时间不变且不进入 HTTP 下载阶段；重新创建传输对象能找回缓存；等长度文件内容损坏后重新下载并恢复正确摘要。测试启用 QStandardPaths::setTestModeEnabled，损坏实验只影响 qttest/cache 下专用副本。已查看正常窗口、800×600 窗口和发送端截图。新增测试账号 15/16（av6_b2567487_a/b），只向专用账号发送，发布文件保留。日志 build/video-chat-smoke/run-cache-layout.log，截图 stage6-chat.png、stage6-chat-small.png、stage6-sender.png；未扩展到音频设备验证。README 和第六阶段知识文档已同步缓存与进度窗口 API，知识文档不放链接。

2026-10-03 播放器源码归属：按 tianmu_sama 要求，Qt 客户端在 E:/chat_server_qt/ChatClient/player 内独立保存 media_window、media_decoder、audio_output、video_view 的四组头文件/实现及 media_playback.pri。主工程和 tests/video_chat_smoke.pro 改用本项目相对路径，不再读取 CHAT_PLAYER_ROOT 或 ChatServer/lessons/video_player。原第五阶段教学示例保留，今后聊天客户端的播放器修改以客户端 player 目录为准；README 与第六阶段知识文档已同步。

本轮为原样复制源码及工程引用调整，没有修改播放器行为。逐文件核对 9 个文件文本完全一致，C++ 文件 UTF-8 BOM 保留。按原 Makefile 中的 Kit、spec 与 CONFIG 分别运行 qmake：生产目录使用 F:/Qt/5.15.2/mingw81_64/bin/qmake.exe -o Makefile ../../ChatClient.pro -spec win32-g++ CONFIG+=debug CONFIG+=qml_debug；联调目录使用同一 qmake -o Makefile ../../tests/video_chat_smoke.pro -spec win32-g++ CONFIG+=debug。两者通过，生成的 Makefile.Debug 均解析到客户端 player，静态搜索无 ChatServer 或 CHAT_PLAYER_ROOT 依赖。本轮没有编译或运行网络测试，也没有关闭正在运行的客户端；现有 EXE 尚未因源码归属调整而重建。

2026-10-03 第五阶段调用链补充：按 tianmu_sama 要求，在第五阶段文档第 8 节原 API 入口位置补全 OpenFile 自动播放链路：GUI 清理并进入 Buffering、排队 OpenRequested、后台打开容器和解码器、Opened 返回后协商输出采样率并请求读取、Read/Send/Receive/转换、BatchReady 回到 GUI 缓存、TryStart 启动以及 Tick 输出。明确 OpenFile 异步返回、show/raise 只管理窗口、一次 Read 是一次读包、数据缓存与实际输出分开。整合到原有章节，不追加问答流水或链接；仅文档编辑，静态核对当前客户端 player 实现。

2026-10-03 第六阶段服务端知识整理：应 tianmu_sama 要求，将前述问答按概念整合进 06-聊天视频收发与播放.md。第 1/2/4 节补充模块与独立 TCP 通道的区别、beginUpload 与实际异步 PUT、generation 和 QNetworkReply；第 6–9 节记录 ChatService 登录身份及参数检查、26/27/28 分发、descriptor/token/Ticket 的生成与一次性上传授权、accept/serve/HTTP parser/64 KiB 缓冲/工作线程写盘、Media 与 History 的关联、业务 publish 与 Redis 发布、消息元数据推送，以及上传异常的尽力清理和向 serve 传播。按实际源码核对字段、状态和 API，特别明确 Token 关联的是聊天登录连接、need_buffer 不等于失败、failed 与 canceled 区别、清理不是磁盘和数据库的原子回滚。知识文档无链接，不写问答或操作流水；同步概述中的本地缓存行为。

2026-10-03 第七阶段前路线讨论：tianmu_sama 希望后续重心转向贴近实际 C++ 流媒体工作的服务端内容，既学视频平台/直播分发（点播、多清晰度、推拉流、并发），也学 RTSP 摄像头监控相关内容；原客户端性能与其他音视频扩展保留后置。本轮处于路线讨论，不开始阶段七实现。用户期望视频未缓存时提供在线播放与下载后播放，已有缓存时复用本地文件；在线播放应不主动保存视频文件，但仍需有限内存缓冲。后续方案需区分文件在线点播与按实时媒体时间线产生的直播，区分转协议/转封装与多清晰度转码。已查官方 SRS、ZLMediaKit、FFmpeg HLS 和 HTTP Range 文档，拟讨论“点播与多清晰度服务”及“直播、摄像头接入与分发”两篇，阶段编号和具体实现边界仍待讨论确认；不把建议当作已批准架构。

2026-10-03 路线范围澄清：tianmu_sama 不要求专门学习摄像头接入，实际诉求是深入 RTP/RTCP。当前路线讨论移出摄像头业务、ONVIF 和 GB28181；拟把第 11 阶段改为使用现有文件的 RTP/RTCP 传输实验，学习 RTP 包头、序号和时间戳、媒体分包重组、乱序丢包与抖动缓冲、RTCP SR/RR 和音画时间映射。录像回放保留为服务端能力，RTSP 仅在需要时解释会话控制关系。主路线文件尚待讨论定稿。

2026-10-03 第七阶段方案讨论：tianmu_sama 明确要求基于现有 ChatServer 和 Qt ChatClient 直接点播聊天视频。只读核对发现 MediaService::download 当前固定 200 并以 64 KiB 读写完整文件，serve 未传递 Range；read 签发五分钟、关联聊天登录会话的 GET 凭证。MediaDecoder::Open 当前用 QFileInfo 限制本地文件；MediaWindow 已有解码线程、150 ms 预取及图像/PCM 有界缓存，但 Tick 未实现网络断粮后的显式重缓冲。拟采用现有 GET 增加单段 HTTP Range，FFmpeg 直接打开带授权头的 HTTP 输入，聊天模块负责鉴权和凭证刷新，播放器复用解码/同步/seek；在线不主动落盘，下载播放和已下载缓存复用继续保留。实现需要补网络取消/超时、缓冲耗尽暂停时钟、恢复播放、五分钟凭证过期后恢复当前位置；按真实代理链验证字节范围和 moov 位于尾部的 MP4，确认首帧不依赖完整下载。此为待讨论方案，不启动业务实现。


### 2026-10-03 第七阶段实现与验证

用户授权按已讨论的现有 GET Range + FFmpeg HTTP 输入方案实施，要求文档分服务端/客户端，本地与在线差异集中展示。途中用户进一步要求没有音频设备也播放带音轨视频，并已连接耳机；这是本轮明确新增要求。

- [x] 服务端解析单段 Range，200/206/416 和错误边界，64 KiB 有界读写；本机 WSL 编译后部署。
- [x] Qt 在线/下载入口、MediaSource 输入类型、授权刷新、网络超时取消、重缓冲，以及无音频设备时单调时钟播放。
- [x] 必要真实联调、知识文档、客户端说明及部署交接。

主要边界：不提供 HLS、直播、摄像头接入或输出设备热切换。客户端 player 目录独立维护，不改第五阶段示例。在线播放不主动生成文件；完整下载缓存仍按摘要校验复用。复用已有业务权限与五分钟 Ticket，Range 每个请求校验。FFmpeg HTTP 每次请求 256 KiB；暂停可以仍有有限在途数据。队列仍有 64 MiB/180 帧和 PCM 两秒保护，未拓展通用队列架构。

服务端真实隔离测试 test_byte_ranges/test_flow 共 2 项，55.96 秒通过。服务器 Full build passed，94.60 秒，运行版本与备份均为 20261003-135708-range，无迁移；部署后多次 active/running、NRestarts 0。Qt 最终增量 Full build passed，9.93 秒，19,098,506 字节 PE x64；运行库部署成功。原既有编译警告未扩大修改。本轮未进行 Git 写操作。

验证覆盖原无音轨 MP4、真实公网 AAC 双声道样本、实际五分钟授权过期恢复、无音频后端无声播放、真实字节转发器制造的断粮/恢复和阻塞 seek 切换本地。Qt 已识别耳机并走音频输出时钟；未声称人工听过声音。首帧前仅传输 563,744 字节，文件全长 4,372,373 字节；请求顺序为文件头、尾部 moov、媒体正文。两份既有样本均是尾部 moov，文档解释 faststart 的索引前置差异，不冒称已实测 faststart 文件。

取消实验产生 FFmpeg Immediate exit requested 与对应 partial file 诊断，已捕获匹配并验证随后本地视频正常起播，没有忽略异常。首次转发器把日志交给持续运行的 PowerShell 重定向，终止时未保留记录；根因是日志落盘路径不可靠，改为辅助程序逐条写 JSONL，补做实际音频模式验证后拿到字节证据。临时隔离服务和转发器完成后关闭。专用联调账号/已发布文件按已有测试约定保留，不触碰真实聊天用户。

知识文档 `07-HTTP视频点播.md` 只记录概念、数据关系及 API，不放链接。详细构建、产物哈希与验收证据另放 STAGE7_DELIVERY.md。后续讲解建议从 media_chat → MediaSource/OpenInput 与服务端 download 两侧对照开始。

2026-10-03 第七阶段 OpenInput 教学补充：用户指出参数 API 与 HTTP 接入原理讲解不足。按安装的 FFmpeg 8.1.1 头文件和实际代码补充 3.2 节：AVDictionary 字符串参数/二级指针/复制与释放、avformat_open_input 四个参数及 mov 容器选择、AVIOContext::pb 字节接口、客户端发请求与服务器回响应、av_read_frame 按需驱动 I/O、解码线程可能阻塞及中断。明确在线授权/缓冲/刷新仍在 OpenInput 之外。仅知识文档修改，保留无链接格式。

2026-10-03 后续路线重排：tianmu_sama 授权调整 audio-video-learning-plan.md，要求贴近 C++ 流媒体实际工作并询问合并媒体处理/HLS。已将两者合成阶段 8，内部 8.1 转封装、8.2 转码、8.3 HLS 打包、8.4 任务与分片授权、8.5 Qt 选档和 ABR 对照；阶段 9 直播推流/接入/分发，阶段 10 RTP/RTCP，阶段 11 并发/故障/资源工程。部署改为贯穿性约束。后置客户端队列、渲染、完整无缝 ABR、低延迟与实时通信，仍不引入摄像头业务。路线查阅 FFmpeg HLS、SRS、ZLMediaKit、RTP/RTCP 与 H.264 RTP 官方资料；教学先直接调用 libav C++ 链路，业务通过受限工作程序隔离转码，初期在本机 WSL 处理，云端轻量分发。直播优先 SRS 基线加核心分发源码带读，不要求重写完整 RTMP。保留前七阶段正文，校正文件开头的已完成状态；本次仅调整计划，不启动阶段八代码或部署。

2026-10-03 路线粒度纠正：tianmu_sama 明确要求第八阶段最多分两部分，并要求缩短响应时间。计划已收拢为 8.1 媒体处理（同一 C++ 工具完成转封装/两档转码）与 8.2 HLS 点播接入，不再以五个小节分别推进。首版只保留单任务处理、基本状态/资源边界与发布鉴权，完整任务领取调度和恢复留到阶段 11。后续只围绕当前问题做必要查阅和定点编辑，不重复研究已核实内容。

2026-10-03 阶段 8.1：按 tianmu_sama 授权完成 lessons/media_process，同一 C++ 工具提供 MP4/MKV remux 与串行 480p/360p 转码（libx264 YUV420P、AAC 48 kHz stereo）。文件操作复用 Qt Core，媒体处理直接调用 FFmpeg；未引入 HLS/业务任务框架。头文件逐函数/成员注释，C++ UTF-8 BOM；知识文档 08-媒体处理与HLS点播.md 的 8.1 记录输出容器/轨道、编码 API、CRF/VBV/GOP、时间基、PCM FIFO 和排空，不放链接。原件和已有目标受保护，半成品 .part，正常异常尽力清理，硬退出不承诺回滚。普通 SDR/连续音轨为范围，复杂颜色、旋转及时间线修复不扩展。

必要验证：MinGW 8.1 x64 Ninja Debug 最终 Full build passed，3.50 秒，2,063,347 字节、PE x64，构建及样本结果在 README，详细真实 API/命令验证在 build/verification.json。9 个输出全量解码及 2.1 秒 seek 后解码无错误；Sintel 1253 帧、无音轨样本 40 帧；MP4 moov 前置；按轨道核对转封装包 SHA256/PTS/DTS/duration 一致。44.1 kHz mono、延后约 0.5 秒的真实片段验证重采样/FIFO 短尾。错误路径验证仅已存在输出和缺失输入，原件摘要未变。没有人工听完整输出，不声称主观质量验收。

排障经验：当前 MinGW 8.1 标准库 filesystem 的 Windows path 实现存在头文件编译错误，使用与第四阶段一致的 Qt Core 文件 API 后通过，不改装编译器。转封装验收不能要求跨轨道全局包顺序完全相同：同 DTS 的音/视频包允许重新交错，应该逐轨道核对顺序、时间和摘要。8.2 尚未开始，教学建议先从 remux.cpp 的 Remux 与 Output::WriteHeader/WritePacket/Finish 读起，再看 TrackTranscoder。继续遵守用户不做 TDD、仅必要验证和缩短响应时间的要求。

2026-10-03 阶段 8.1 知识整理：按 tianmu_sama 要求将近期问答合并到 08-媒体处理与HLS点播.md 原有概念章节，补转封装用途、编码格式取舍、封面流/轨道元数据、WriteHeader/faststart/Range、串行多档与 ConfigureVideo/ConvertVideo/Encode 职责、分辨率和画质、变长编码包及音视频交错、逐级排空与持续写盘。明确更正：不能把解码依赖或延迟造成的多次送包后才输出，直接解释为同一编码帧拆在多个 AVPacket；区分媒体包、网络分片和编码帧组织。无问答流水、无链接，8.2 仍待实现；静态检查段落归属和代码围栏，不扩展测试或重新构建。

2026-10-03 阶段 8.2 方案补充：tianmu_sama 要求服务端告知可用清晰度，前端按最高可用档位及以下显示，480p 视频不能出现 720p。已更新路线：候选 720p/480p/360p，按源分辨率选择、不向上放大；服务端返回实际已发布就绪的档位列表，最高可用值从列表得出，前端动态生成选项并与 HLS 主列表保持一致，不能只根据原件尺寸推测尚未生成的档位可播放。当前仍为 8.2 方案讨论。


### 2026-10-03 阶段 8.2 执行
用户确认开始 HLS 点播，特别要求用实际数据解释 HLS/TS/PES 与 HTTP/TCP 包结构；动态档位按已就绪列表，480p 不显示 720p。实施沿用本机媒体处理与 WSL 编译、云端分发；采用本机受控发布工具准备资源包，以独立目录目录清单记录派生状态，不把转码放进聊天请求线程。
- [x] C++ HLS 打包及 TS/fMP4 产物、动态档位与包结构观察。
- [x] 服务端派生清单、整套 HLS 鉴权读取与本机发布入口。
- [x] Qt HLS 输入、动态档位切换与定位恢复。
- [x] 本机构建、必要真实验证、云端部署及专业知识文档。

完成记录：media_process hls 串行转码后调用 HLS muxer；Sintel TS 为 480/360、无声 fMP4 为 720/480/360。ConvertVideo 按媒体时间强制 IDR（forced-idr=1），多档关键帧 PTS 对齐。publish_hls.py 使用本机 Paramiko、源/产物 SHA256、排他发布锁、不可变 revision 目录及原子 catalog 指针；没有自动上传后任务调度，失败留下的目录/锁及旧版本人工处理，阶段 11 再扩展。read/playback 返回实际 variants，所有 HLS 资源逐次验证短期凭证、登录会话和聊天权限；原件下载继续 read/original。Qt Kind::Hls 选择 hls 解封装器、下拉框选媒体列表，OpenSource 后 seek 恢复位置/暂停意图，AuthorizationRequested 携带档位。

必要验证：所有 TS/fMP4 档完整解码、关键帧/尺寸检查；真实隔离服务 test_hls_assets 62.337 秒通过；公网 Qt 实际首帧、2.3s 暂停 seek、切360p、提前触发客户端续签后保留档位通过。续签测试没有等待服务端五分钟到期，不夸大为后端过期实测。fMP4 seek 出现 3 次重复 MOOV 警告，核对 FFmpeg 8.1.1 hls_read_seek 重取 init 与 mov_read_moov 已读则跳过的路径，捕获日志并白名单核对，非损坏包；未屏蔽诊断。没有主观试听结论。

构建与部署：三主目标 Full build passed，media_process Debug x64 7.48 秒（含配置）、Qt ChatClient Debug x64 9.13 秒、WSL ChatServer Release x86_64 96.26 秒。云端运行 /home/lth/chat_server/releases/20261003-210856-hls/ChatServer，备份同名目录，未迁移数据库；PID76495/NRestarts0/约23MiB为验证快照。已给12条匹配两份样本的媒体记录准备HLS，包括测试账号25/26的两条；准备既有样本无新聊天通知。细节、摘要及命令在 STAGE8_DELIVERY.md。测试执行更新了仓库原先跟踪的 tests/__pycache__/media_schema_test.cpython-312.pyc；未进行Git恢复，后续执行设置 PYTHONDONTWRITEBYTECODE。

排障记忆：服务器使用自带 json.hpp，不能写系统 nlohmann/json.hpp；Qt toArray 必须包含 QJsonArray，单独测试需要 tcpclient.h 的完整定义。新测试插入原方法前时不要挪用它的 skipUnless 装饰器。Client UUID 使用标准带连字符形式，begin_file 对任意32位hex不认可；上传测试务必将文件正文传入 transfer。下次直接从08知识文档8.2及hls_package.cpp讲起，不重复构建部署或扩展阶段9。

### 2026-10-06 分片基础补充
用户需要单独深入解释 HLS、m3u8、TS、fMP4 的关系与存在原因，侧重实际字节和播放过程，知识文档不放外部链接。已新增《08-补充-分片与HLS基础.md》，结合现有产物的播放列表、TS PID/PES 字节、fMP4 box 和 tfdt 解释资源层、容器层、编码层。只读核对：Sintel 首片 94,376 字节等于 502 个 TS 包；无声 720p init 为 850 字节，第二片 tfdt=180000、timescale=90000。强调分片容器不是在线播放的普遍前提，初始化信息、随机访问条件与独立文件是不同概念。文档工作不构建或部署。

### 2026-10-06 服务端媒体处理范围澄清
用户明确：小内存服务器限制的是编译，未要求本地转码；期望客户端上传原件后由服务端后台自动转码、分片并登记档位。此前把本机手动打包发布当作阶段 8.2 完整链路，遗漏了自动处理环节，不能再将其描述为已完成该目标。后续实现应围绕服务端处理推进；编译继续在本机，转码运行资源单独实测，不能仅凭它是普通可执行程序保证资源足够。本轮用户询问做法是否符合实际业务，先解释，不擅自启动改造。

### 2026-10-06 自动点播处理实施
用户授权把上传后处理接到服务端并部署，参考实际点播工作流；Linux 修改不使用 cpp-utf8-bom 技能。
- [x] 复用媒体处理工具，在本地 WSL 构建 Linux 运行程序。
- [x] 原件提交与持久任务入队同一数据库事务；后台串行执行、失败记录/重试、重启恢复、完整产物原子发布。
- [x] 必要的真实上传/分片/读取与故障验证，本地编译后部署公网服务器。
- [x] 修改阶段 8.2 知识文档、分片补充与运行交接。
运行架构：ChatServer 管理 MySQL 持久任务，独立子进程执行现有 C++ media_process HLS 处理；单机一个任务同时运行，三个档位逐档处理。公网机器 Ubuntu24.04 已有 FFmpeg6.1 和 Qt5Core 运行库，约722MiB可用（预检快照）；不在远端编译。不引入独立消息中间件，文档说明与大型分布式工作流的对应关系。

验证进展：TS 三项真实服务验证通过（101.606秒）；最终版本 fMP4 同样三项通过（108.015秒），覆盖上传自动处理、失败保留原件与授权重试、处理中重启恢复。Linux最终ChatServer构建42.68秒（主机命令46.59秒），media_process5.67秒，Qt ChatClient Debug/x6420.37秒。期间Qt smoke编译发现GNU make拾取PATH中的sh.exe吞掉Windows反斜线，按仓库README限制PATH后通过8.92秒；不更改项目工具链。一次fMP4 fixture启动超时发生在本机构建并行期间，单独运行最终产物后通过，没有延长超时或改生产代码掩盖。用户已经授权本次部署；公网切换前备份和004任务表迁移进行中。

完成记录：已部署 20261006-115359-media-jobs，备份同名目录，004任务表迁移已执行，数据库备份在独立临时库验证恢复。既有12份HLS复用，新上传两份由服务器实际处理：Sintel33.2秒生成480/360，原无声视频6.08秒生成720/480/360。最后14个任务均ready；active/running、PID87336、NRestarts0，cgroup峰值269946880字节（约257MiB，含子进程）。公网Qt首帧、档位、2.3秒暂停seek、切360p和真实重新授权通过；没有把客户端缩短期限实验说成后端五分钟过期验证。

末次诊断：独立Qt测试目录未部署插件，首轮启动没有进入测试且退出1；显式设置QT_PLUGIN_PATH/QT_QPA_PLATFORM_PLUGIN_PATH后测试通过，无需改产品代码。测试日志逐行核对：两条通过结果、正常分片打开，以及一次窗口析构主动中断HTTP的Immediate exit requested；无其他诊断。测试口令fixture已删除，专用账号27/28及其测试消息保留，不触及真人会话。最终报告STAGE8_AUTOMATIC_VOD_DELIVERY.md记录构建命令、摘要、上线位置和运行限制。知识文档与分片补充保持自包含无链接，路线和README纠正本机转码要求。

实现注意：原子rename成功后的目录fsync仍可能失败，不能因此删除已被catalog引用的产物；资源复制失败的清理只围绕catalog发布前。Qt头文件构造注释中的program参数不存在，最后仅改为从CHAT_MEDIA_PROCESS读取，静态核对而不重复编译。Git diff --check两仓库通过，提示的LF/CRLF是仓库换行策略，不进行无意义全文件改写。

### 2026-10-08 HLS 切档片尾定位修复（仅源码）
用户报告多次切档出现 av_seek_frame: Operation not permitted。查阅实际 FFmpeg 8.1.1 hls_read_seek/find_timestamp_in_playlist 与 seek_frame_internal，确认 HLS first_timestamp 取首包 DTS；定位失败经过 AVFMT_NOGENSEARCH 分支返回 -1，错误文字不能视为 HTTP 权限拒绝。修改前直接调用本机同版本 DLL 对既有 hls-silent/720p/index.m3u8 进行只读诊断：start_time=0.25秒、duration=5秒，播放器4.7秒成功，4.75/4.8/4.999/5秒均返回-1，复现截图错误。尚未得到用户对是否仅片尾触发的回答，不能声称排除全部快速切档竞态。

用户明确允许先改，但虚拟机设置改变，禁止本轮编译/上线。仅修改Qt项目 media_decoder.h/.cpp 和 media_window.cpp：单档HLS借读保留首包，以其DTS设seek_origin_us_；origin_us_保留显示时间职责；首次Read消费预读包，ResetDecodeState清标志和引用；Opened按输入时长限制恢复位置后再建立音频锚点。非HLS沿用容器起点。新增成员有中文注释。约25行功能改动，静态核对调用、代次过滤、包所有权、成功seek清理与位置边界；diff --check通过，未编译、未运行修改后测试、未部署，现有EXE仍未包含修复。知识文档同步补充起点区别。后续用户授权编译时再验证TS/fMP4反复切档、4.75秒之后、播完切档、暂停与续签场景。
