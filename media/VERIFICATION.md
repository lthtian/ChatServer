# 图片功能验证记录

日期：2026-09-17。核心、仓储、TCP/HTTP 图片链路、Electron 持久任务及实际窗口已验证。OSS、磁盘满、强杀及高并发压力验收仍待进行。

## 图片全链路验收

Linux x86-64 / Release，目标 `ChatServer`，最后一次源代码编译 **Full build passed**，48.28 秒；无编译警告。命令：

```sh
cmake --build /opt/chat-build/build/server-Release --target ChatServer --parallel 2
```

产物 `D:\chat\_server\Output\linux-x64\Release\ChatServer`，51797568 字节，2026-09-17 16:13:41（UTC+8），ELF x86-64。SHA256：`f2721c5ffdae42af7f78e31cc524914b5aca50f21bb206c447bec9413908a2a2`，远程副本一致，动态库可解析。

| 验证 | 结果 |
| --- | --- |
| C++ 图像/磁盘/CLI | CTest 14/14，4.27 秒，包含过期暂存回收与活跃暂存保留 |
| 真实 MySQL 迁移/仓储 | 5 组通过，44.744 秒，包含失败/过期重试和回收候选 |
| WSL TCP/HTTP + Windows 客户端 | 7 项通过，50.200 秒；默认 Redis 配置，真实 C++ 程序、MySQL、Redis |
| 远程隔离库、远程二进制 | 6 项通过，4.626 秒；1 项 Windows 专属测试按平台跳过 |
| Electron 内置 Node | TCP 3、流式传输 2、C++ 子进程 5 项全部通过 |
| 实际 Electron 窗口 | 登录、图片历史、缩略图、原图、关闭预览通过；页面无 require/process，控制台无错误 |

集成覆盖：未登录/非好友/非群成员拒绝、私聊和群聊图片发布、重复发布、真实 ACK 未读取后断线重发、游标历史、坏摘要、取消、上传中断重试、过期续期，以及客户端原文件删除后恢复任务、缓存原图损坏后重新下载和取消下载清理。

```sh
systemctl start mysql redis-server
CHAT_TEST_DEFAULT_REDIS=1 CHAT_WINDOWS_CLIENT_TEST=1 \
CHAT_SERVER_TEST_BINARY=/opt/chat-build/source/bin/ChatServer \
python3 /mnt/d/chat_server/ChatServer/tests/media_service_test.py
```

`CHAT_WINDOWS_CLIENT_TEST` 仅用于当前 Windows/WSL 工作空间。远程不设置该项，测试自行创建和清理随机临时库、数据库用户、临时 Redis 和图片目录。`CHAT_TEST_DEFAULT_REDIS` 只用于没有业务数据的本地 WSL Redis，不能用于共享生产 Redis 的隔离测试。

修复并回归了两类启动/关闭问题：默认 Redis 参数在协程调用处触发空字符串构造；静态连接资源晚于 io_context 释放导致退出异常。默认配置完整用例已通过，测试检查服务退出码及异常日志。首次上线发现默认配置问题后恢复原服务，再以修正产物部署；生产库迁移前备份在服务器私有目录，历史行数 106 保持不变。

独立仓储目标仍产生 Boost 1.83 系统头的两条 `-Wmaybe-uninitialized`，具体分析见下方记录；没有屏蔽告警。测试捕获到隔离 Redis 的系统 overcommit 提示，测试关闭持久化且不执行 fork 保存；未为测试修改系统内存参数。

运行与客户端交互细节见 `docs/chat-image-client-details.md`；下方保留独立核心及最初 WSL 环境验证记录。

## 本机构建

环境：Windows x64，Visual Studio 2022 Community / MSVC 19.44，OpenCV 4.13.0，Release。三个 EXE 的 PE machine 均为 `0x8664`。

构建工作目录：

```text
C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin
```

从已有 CMakeCache 核实生成器、平台和 CMake 路径后执行：

```bat
cmd.exe /d /c cmake.exe --build "D:\chat_server\ChatServer\media\build\native" --config Release --target chat_image image_test disk_store_test --parallel -- /p:TrackFileAccess=false
```

结果：**Full build passed**。所有目标生成完成；最终增量构建无编译警告。命令异步返回，未记录三目标构建的精确总时长；随后 `chat_image` 单目标增量构建也完成，首次返回等待为 10.02 秒，总时长未单独计时。

核实的输出：

| 目标 | 输出文件 | 大小 | 修改时间（本地） |
|---|---|---:|---|
| chat_image | media/build/native/Release/chat_image.exe | 112128 B | 16:35:45 |
| image_test | media/build/native/Release/image_test.exe | 37376 B | 16:32:12 |
| disk_store_test | media/build/native/Release/disk_store_test.exe | 91648 B | 16:32:20 |

测试命令：

```powershell
$env:PATH = 'D:\chat_server\ChatServer\media\build\deps\opencv\build\x64\vc16\bin;' + $env:PATH
& '.\ctest.exe' --test-dir 'D:\chat_server\ChatServer\media\build\native' -C Release --output-on-failure
```

CTest 14 组通过，完整执行 0.94 秒。之后为确认编解码日志不会破坏 JSON 契约，增加损坏 PNG 用例并把 CLI 错误结果放到 stdout；相关 CLI 组再验证通过，0.49 秒。该组包含 6 个真实进程/文件用例。核心库其余源码未因此变动。

TDD 记录：图片核心 7/8 场景先失败后通过；磁盘 5/5 先失败后通过；CLI 5/5 先失败后通过，再补充诊断日志用例。磁盘测试暴露 Windows 根盘符 `C:` 被当作普通目录创建，定位后改为从 `root_path()` 遍历 `relative_path()`，复测通过。

## Electron 调用

实际使用客户端安装的 Electron **28.3.3**，设置 `ELECTRON_RUN_AS_NODE=1` 运行 `tests/image_processor.test.js`，没有启动聊天 UI。5/5 用例先失败后通过，最终耗时约 0.47 秒。覆盖真实 C++ EXE、中文/空格/`&` 路径、损坏输入、原生日志、预先取消和找不到处理程序。

Windows GUI 子系统可执行文件经 PowerShell 直接启动时没有返回可用测试输出，因此通过 Python `subprocess.run(..., capture_output=True, timeout=30)` 等待并捕获真实退出码/TAP；未将无输出的退出视为测试通过。

## Linux 核心与数据库

服务器：Ubuntu 24.04，GCC 13.3，CMake 3.28.3，OpenCV 4.6.0，Boost 1.83，MySQL 8.0.46。项目目录 `/home/lth/chat_server/ChatServer`。

已读取 History、User、Friend、GroupUser DDL，并经授权在 `/tmp/chat-media-test-OSQKI2vv` 执行隔离测试。

```sh
cmake -S media -B media/build/linux -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build media/build/linux --target chat_image image_test disk_store_test --parallel 2
ctest --test-dir media/build/linux --output-on-failure
python3 tests/media_schema_test.py --sudo -v
```

Linux x86-64 Release 三目标 **Full build passed**，编译 19.730 秒，无编译警告。`chat_image` 153824 B、`image_test` 45152 B、`disk_store_test` 84768 B，ELF 64-bit x86-64；输出时间分别为 16:52:50、16:52:54、16:52:57（UTC+8）。CTest 14/14 通过，1.74 秒。

迁移测试先在空实现上得到四项预期失败，再应用 `001_chat_media.sql`，四项通过，0.910 秒。覆盖文本历史保留、图片引用约束、发送者幂等唯一约束及媒体列。数据库名称带随机 `chat_media_test_` 前缀，测试结束删除；未对业务库应用迁移。

数据库操作层另用 `tests/media_repository_test.cpp` 验证媒体状态、权限和消息事务。其初始三场景均因尚未实现而失败，后续结果在该层完成验证后补充。

## 数据库操作层构建中断

`cmake --build tests/build --target media_repository_test --parallel 1` 的 Release 实现构建未完成，17:17 用户重启服务器。该次结果为 **Environment blocked; source compilation unverified**，不能把先前测试占位实现的可执行文件视为当前实现构建通过。

重启前 17:11–17:15 的 systemd-resolved/systemd-journald 日志反复记录 `Under memory pressure, flushing caches`，两次新建 SSH 连接在 banner 阶段超时；没有查到该时间段 OOM kill 记录。系统统计最后一个重启前采样是 17:10，尚未覆盖卡顿阶段，不能据此量化故障时的 swap 或编译峰值。

重启后实测内存总量 1612 MiB、可用约 879 MiB，swap 4095 MiB、已用 0；MySQL RSS 398384 KiB，MySQL 和 Redis 均 active，未发现残留编译进程。暂停直接在该服务器执行无内存限制的编译；降低并发本身不足以限制单个编译单元的内存占用。

## WSL2 本地 Linux 验证

2026-09-17，ChatUbuntu / Ubuntu 24.04.5，普通开发用户 `lth`（UID 1000）。发行版位于 `F:\linux\_environment\Ubuntu-24.04`，内存上限 8 GB、4 个处理器、2 GB swap。GCC 13.3、CMake 3.28.3、Boost 1.83、OpenCV 4.6、hiredis 1.2、OpenSSL 3.0.13、MySQL 8.0.46，与远程服务器的主要构建依赖版本一致。

Windows 原始源码从 `D:\chat_server\ChatServer` 同步到 `/opt/chat-build/source`，使用独立 Linux CMake 构建目录。`F:\linux\_environment\build-chat.ps1` 支持 `server`、`media`、`media-tests`、`repository` 目标及 Release/Debug 配置；此次仅验证 Release。

```sh
cmake --build /opt/chat-build/build/media-Release --target image_test disk_store_test chat_image --parallel 2
cmake --build /opt/chat-build/build/tests-Release --target media_repository_test --parallel 2
cmake --build /opt/chat-build/build/server-Release --target ChatServer --parallel 2
```

三个命令均为 **Full build passed**，平台 Linux x86-64。底层编译耗时分别为 12.63、30.01、66.74 秒，不含首次 CMake 配置及文件同步。完整 ChatServer 构建中，`time -v` 报告最大单进程 RSS 1494216 KiB（不是所有并行进程的总峰值）；运行采样中 swap 使用为 0，MySQL 同时运行。

| 目标 | 核实路径 | 大小 | 修改时间（UTC+8） |
|---|---|---:|---|
| ChatServer | D:\chat\_server\Output\linux-x64\Release\ChatServer | 30170632 B | 10:09:16 |
| chat_image | D:\chat\_server\Output\linux-x64\Release\chat_image | 153824 B | 10:04:47 |
| image_test | /opt/chat-build/build/media-Release/image_test | 45152 B | 10:04:41 |
| disk_store_test | /opt/chat-build/build/media-Release/disk_store_test | 84768 B | 10:04:42 |
| media_repository_test | /opt/chat-build/build/tests-Release/media_repository_test | 1112392 B | 10:05:53 |

所有产物均为 ELF 64-bit x86-64。`ChatServer` 和 `chat_image` 的 Output 副本与 Linux 构建产物逐字节一致，动态链接依赖均可解析。项目根 CMake 为服务端添加了 `-g`，因此 Release 产物包含调试信息。完整服务端仅编译，未启动业务应用。

```sh
ctest --test-dir /opt/chat-build/build/media-Release --output-on-failure
systemctl start mysql
python3 /opt/chat-build/source/tests/media_schema_test.py --repository-test /opt/chat-build/build/tests-Release/media_repository_test -v
```

CTest **14/14 通过，5.71 秒**。数据库命令以本地 WSL root 执行，真实 MySQL **5 组全部通过，32.835 秒**。仓储组含 lifecycle、permissions、cancel、failure、expiry、group、prepare 七个场景，以及八个独立进程同时发送同一媒体消息、最终仅一条 History 记录的检查。覆盖重复申请及参数冲突、状态条件更新、取消、过期、发布幂等、好友/群成员访问与历史游标；不包含尚未接入的 TCP 登录态和 HTTP 上传。测试结束再次查询，临时数据库和账户数量均为 0。

首次数据库测试在 WSL 冷启动过程中启动过早，因 MySQL socket 尚未就绪而在 setUpClass 阶段失败，实际执行 0 项测试；服务启动同步完成后运行上述命令通过。测试入口保留 `systemctl start mysql`，等待服务 readiness，不通过固定休眠猜测启动耗时。

仓储测试编译在 Boost 1.83 的系统头 `protocol.ipp:576` 报告两条 `-Wmaybe-uninitialized`。已检查 `serialization.hpp` 中 int_lenenc 反序列化的赋值分支及调用方错误返回路径；未修改系统头或屏蔽告警。完整 ChatServer 编译未产生告警。原始构建和测试日志位于 `F:\linux\_environment\logs`。
