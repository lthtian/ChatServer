# 普通文件功能验证

日期：2026-09-21。本地验证使用 WSL 隔离数据库和实际 TCP/HTTP 服务；同日已应用 003 并部署生产。

## 构建

目标 ChatServer，Release，Linux x86-64。命令：

```powershell
wsl -d ChatUbuntu -u lth --exec bash /mnt/f/linux/_environment/build-chat.sh server Release
```

脚本使用现有 Unix Makefiles 构建目录，执行 `cmake --build /opt/chat-build/build/server-Release --target ChatServer --parallel 2`。

**Full build passed**，75.07 秒。输出已更新：`D:/chat/_server/Output/linux-x64/Release/ChatServer`，52,525,384 字节，2026-09-21 10:08:56。
SHA-256：`7bcf6bbf630abb0c18b9f1df97a338439280b5426fcb0ef912e7d2c666dc52cf`。

## 验证结果

- TDD：实现前 begin_file 真实服务测试因 invalid_argument 失败；实现后通过。
- 客户端本地 20/20 通过，1.193 秒，使用实际 SQLite、磁盘和 HTTP。
- `python3 -m unittest file_service_test message_service_test shutdown_test -v`：20 项中 18 通过、2 Windows 专属项跳过，188.482 秒。包括空文件、超限拒绝、文件名校验、群聊/私聊、授权、历史、幂等、图片回归、文本确认和退出。
- Windows 实际客户端与窗口第一轮通过，103.122 秒。21 MiB 二进制与空文件上传、删原路径、缓存接管、强制回源、损坏缓存恢复、取消下载、离线命中和独立保存均验证。
- 窗口截图和日志：`F:/linux/_environment/logs/client-storage/`。

前端说明与链路图：`E:/chat_server_qt/ChatClient/chat_client_electron/FILE_FEATURE.md`。

尚不包含断点续传、OSS、磁盘满/进程强杀和高并发压力验收；普通文件不解析内容或自动执行。

最终 Windows 客户端及在线/离线窗口复跑通过，121.221 秒；新增真实上传取消，确认服务端资源状态 canceled、客户端任务移除。saveAs 的磁盘保存也纳入退出与账号切换等待，客户端本地测试已覆盖操作跟踪。

生产部署：/home/lth/chat_server/releases/20260921-files/ChatServer，运行进程摘要与上述产物一致。备份 /home/lth/chat_server/backups/20260921-files，数据库恢复校验成功；迁移前后 History 110 / Media 2。服务 active/running，NRestarts 0，6000/回环6002监听正常，SSH 16000/16001授权拒绝探测通过。
