# 客户端消息与图片存储验证

## 环境与构建

2026-09-20，WSL2 ChatUbuntu，MySQL 与 Redis 正常运行。所有验证使用随机命名的测试库和测试账号，不连接生产数据库。

目标 `ChatServer`，配置 `Release`，平台 Linux x86-64。通过现有 `F:\linux\_environment\build-chat.sh server Release` 同步源码并调用：

```sh
cmake --build /opt/chat-build/build/server-Release --target ChatServer --parallel 2
```

首次构建 75.54 秒；连接清理修复后的增量构建 25.73 秒，均为 **Full build passed**。

最终产物 `D:\chat\_server\Output\linux-x64\Release\ChatServer`，52,337,648 字节，修改时间 2026-09-20 19:56:08（UTC+8），ELF x86-64。Windows 输出副本与 Linux 构建产物 SHA-256 一致：

```text
0986ffc1cf69505521229395f553b98ee7ceffa0de462a5a5363fde9176928a6
```

## 覆盖范围

最终验收：退出回归与 9 项服务测试通过；客户端集成组单独复跑通过（84.351 秒）。整组首次汇总仅因离线窗口日志包含一个空行而失败；入口允许纯空白后复跑，仍要求无实际错误或警告内容。在线和离线窗口结果文件均为 `{"ok":true}`。

- 客户端本地 19 项：真实 SQLite、文件缓存、本地图片处理工具、TCP 与 HTTP。
- MySQL 5 项：迁移回填、消息约束、会话序号事务回滚与并发写入。
- 服务协议 9 项：文本确认、幂等重发、同步边界、图片传输、授权、取消、错误校验、断传重试和实际 Windows 图片客户端。
- `tests/shutdown_test.py`：未登录 TCP 连接仍打开时，服务端必须正常退出。
- 前端 `tests/message_sync.test.js`：真实服务下实时入库、55 条离线消息补齐、重复同步去重、断线文本重试、58 条记录关闭数据库后重新读取。
- 前端窗口测试：历史缩略图、原图、文本发送确认；已登录后断线仍可读取本地文字和图片。

## 回归与限制

停止服务时，未登录连接曾因没有纳入业务层登录连接表而阻止事件循环退出。独立回归先复现 5 秒退出超时；网络层维护全部会话并在停止时关闭，最终产物的回归通过。

立即重连可能先收到“该用户已经登录”，因为服务端断线状态异步写入数据库。集成测试有界等待并校验该明确响应，再验证同一个客户端消息编号重试，不通过改写数据库模拟离线。

本轮曾有一次测试服务启动超时，原因尚未定位；测试已附带启动日志，未扩大启动超时阈值。此前窗口缩略图偶发超时在本轮未复现，不将其描述为已修复。

2026-09-21 已完成生产部署，详见下方部署记录。

## 复跑入口

```sh
export CHAT_SERVER_TEST_BINARY=/opt/chat-build/source/bin/ChatServer
export CHAT_WINDOWS_CLIENT_TEST=1
cd /opt/chat-build/source/tests
python3 -m unittest -v shutdown_test.ShutdownTest message_service_test.MessageServiceTest client_storage_test.ClientStorageTest
```

Windows 客户端与 Electron 路径由当前测试环境确定，入口使用隔离服务分配的端口，不使用生产 SSH 隧道。

日志与截图：`F:\linux\_environment\logs\client-storage`。

## 生产部署：2026-09-21

- 远程隔离复验 10 项：9 项通过，1 项 Windows 专属测试按平台跳过，耗时 8.426 秒。
- 停服后备份数据库、图片目录和 systemd 单元；数据库备份恢复到独立临时库并核对记录数量后清理验证库。
- 备份目录 `/home/lth/chat_server/backups/20260921-storage`，数据库和图片备份仅 root 可读。
- 已应用 `002_message_sequence.sql`：108 条历史消息均有有效序号，2 个媒体资源保留；会话计数器与历史最大序号一致，触发器存在。
- 正式程序 `/home/lth/chat_server/releases/20260921-storage/ChatServer` 与本文记录的 SHA-256 一致。systemd active/running/enabled，检查时 PID 23039、NRestarts=0。
- TCP `0.0.0.0:6000`，图片 HTTP `127.0.0.1:6002`。本地 16000/16001 隧道已检查，未认证请求被拒绝。当前图片 HTTP 未认证错误使用 400 与 `unauthorized` 响应体；探测断言已按实际接口约定校验。
- 临时测试库与测试账号数量均为 0。未用真实用户账号发送测试消息；正式账号的界面操作由用户体验验证。

客户端在 `E:\chat_server_qt\ChatClient\chat_client_electron` 运行；隧道运行期间只需 `npm run dev`。隧道退出或电脑重启后，先另开终端运行 `npm run tunnel` 并保持运行。
