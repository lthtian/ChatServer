# 本地构建与阿里云服务部署交接

整理日期：2026-10-06。当前自动点播部署见第 13 节；第 11、12 节为此前发布记录。

本文包含本机实际构建脚本、2026-09-21 程序部署记录及公网直连配置。接手时仍应读取服务器实际状态；连接入口与直连配置见第 9 节。

## 1. 先分清交付物

- Qt Windows 客户端：Windows EXE，在 Windows 本机运行，使用该客户端项目自己的构建说明。
- 阿里云服务端：Linux x86-64 ELF 可执行文件 `ChatServer`，无 `.exe` 扩展名。此前是在 Windows 的 WSL2 Ubuntu 中编译，再用 SCP 上传运行。
- 不要把 MSVC 编译的 EXE 上传到 Ubuntu 当作服务端运行。
- 远程机器内存小，曾因 C++ 编译导致卡死；继续本地 WSL 构建，不在远端编译，也不为编译停止远端 MySQL。

## 2. 已知环境与准确路径

| 项目 | 值 |
| --- | --- |
| Windows 服务端源码 | `D:\chat_server\ChatServer` |
| WSL 发行版 / 开发用户 | `ChatUbuntu` / `lth` |
| WSL 环境所在目录 | `F:\linux\_environment` |
| 构建脚本 | `F:\linux\_environment\build-chat.sh` |
| WSL 中 Windows 源码路径 | `/mnt/d/chat_server/ChatServer` |
| Linux 构建工作区 | `/opt/chat-build` |
| 自动同步的源码副本 | `/opt/chat-build/source` |
| Release CMake 构建目录 | `/opt/chat-build/build/server-Release` |
| Linux 原始输出 | `/opt/chat-build/source/bin/ChatServer` |
| 复制到 Windows 的输出 | `D:\chat\_server\Output\linux-x64\Release\ChatServer` |
| 历史部署脚本目录 | `D:\chat\_server\Output\deployment` |
| 服务器 | `39.105.18.142`，SSH 端口 `22` |
| SSH 用户 | `lth`，可以 sudo |
| systemd 服务 | `chatserver.service` |
| 服务单元 | `/etc/systemd/system/chatserver.service` |
| 远端工作目录 | `/home/lth/chat_server/ChatServer` |
| 远端版本目录 | `/home/lth/chat_server/releases/<发布编号>/ChatServer` |
| 备份目录 | `/home/lth/chat_server/backups/<发布编号>` |
| 持久媒体目录 | `/home/lth/chat-media`，此前属主 lth、权限 700 |
| MySQL / Redis | 本机 `127.0.0.1:3306` / `127.0.0.1:6379` |
| 业务数据库 / 应用数据库用户 | `chat` / `lth` |
| 聊天 TCP | 程序监听 `0.0.0.0:6000`；客户端使用 Nginx 公网入口 `39.105.18.142:7000` |
| 媒体 HTTP | 程序监听 `127.0.0.1:6002`；Nginx 通过公网 80 的 `/media` 路径转发 |

注意源码路径是 `D:\chat_server`，输出路径却是 `D:\chat\_server\Output`，两者不要混淆。

此前 WSL 为 Ubuntu 24.04、GCC 13.3、Boost 1.83、OpenCV 4.6、MySQL 8.0.46。主目标使用 C++20，链接 OpenCV、Boost system、hiredis、pthread、OpenSSL；具体依赖以当前 CMakeLists 为准。WSL 配置为 8 GB 内存、4 核、2 GB swap，服务端编译并发为 2。

SSH 和 sudo 通过交互式密码输入。tianmu_sama 已明确要求将账号和密码保存在本地交接记录中，供执行会话读取：`D:\chat_server\ChatServer\context_transfer_report.md` 的“本地交接凭据”一节。已有凭据无需重复索取，不把密码写进命令参数或运行日志。SSH/sudo 密码与数据库应用密码不是同一种凭据。

## 3. 构建前检查与实际构建

PowerShell：

```powershell
wsl --list --verbose
wsl -d ChatUbuntu -u lth --exec uname -m
Get-Content F:\linux\_environment\build-chat.sh
```

预期架构 `x86_64`。如果报 `HCS_E_HYPERV_NOT_INSTALLED`，先处理 Windows 虚拟化启动状态，不重装发行版。2026-10-01 已把 `hypervisorlaunchtype` 从 Off 改为 Auto，需要重启才生效，日志在 `F:\linux\_environment\logs\restore-hypervisor-20261001.json`；这不是已经重启或 WSL 正常的证明。

**确认新服务端源码就在上述源码目录且目标仍叫 ChatServer，才能执行：**

```powershell
wsl -d ChatUbuntu -u lth --exec bash /mnt/f/linux/_environment/build-chat.sh server Release
```

脚本会：

1. 使用 `/opt/chat-build/.build.lock` 防止同时构建。
2. 验证 `.source-origin` 和路径，将 Windows 源码 rsync 到 Linux 副本；使用 `--delete`，排除 `.git/`、`build/`、`bin/`、`openspec/`。
3. 构建缓存不存在时配置；已有缓存时执行：

```bash
cmake --build /opt/chat-build/build/server-Release --target ChatServer --parallel 2
```

4. 成功后复制 Linux 可执行文件到 Windows 输出目录。

不要在 `/opt/chat-build/source` 手工保留开发修改，下一次同步会覆盖它。如果另一会话的服务端换了源码根、依赖或目标名，先适配构建路径和 `.source-origin` 的一致性，不能直接用此脚本编译错误项目。CMake 输入变化也必须核对已有配置，不用清理重建作为默认手段。

构建命令只编译，不自动运行程序或测试。构建失败就停止，不能上传输出目录中残留的旧程序。成功后核对：

```powershell
Get-Item D:\chat\_server\Output\linux-x64\Release\ChatServer | Select-Object FullName,Length,LastWriteTime
Get-FileHash D:\chat\_server\Output\linux-x64\Release\ChatServer -Algorithm SHA256
wsl -d ChatUbuntu -u lth --exec file /mnt/d/chat/_server/Output/linux-x64/Release/ChatServer
```

记录本次哈希、构建耗时、配置和输出时间。预期为 ELF 64-bit x86-64，而不是 PE32/Windows EXE。

## 4. 远端预检：先读取现状

PowerShell 中连接：

```powershell
ssh -tt -p 22 lth@39.105.18.142
```

以下在远端 shell 执行：

```bash
uname -m
cat /etc/os-release
free -h
df -h /home/lth
sudo systemctl cat chatserver.service
sudo systemctl show chatserver.service -p ExecStart -p WorkingDirectory -p ActiveState -p SubState -p NRestarts
sudo systemctl is-active mysql redis-server
ss -ltn '( sport = :6000 or sport = :6002 )'
sudo mysql -NBe 'SELECT VERSION();'
sudo mysql chat -e 'SHOW CREATE TABLE History\G SHOW CREATE TABLE Media\G'
```

不要把服务环境变量中的秘密复制到对话或普通文档。之前可直接 `sudo mysql` 管理数据库，不需要先猜 root 数据库密码。

最后已验证部署记录（2026-09-21，不是当前实时状态）：

- 程序：`/home/lth/chat_server/releases/20260921-files/ChatServer`。
- SHA256：`7bcf6bbf630abb0c18b9f1df97a338439280b5426fcb0ef912e7d2c666dc52cf`，52,525,384 字节。
- 001、002、003 迁移已应用；History.kind 包含 text/image/file，Media 有 kind/name。
- 当时 History 110、Media 2；这些数量只能作历史记录，不能拿来断言现在数据数量。
- 备份：`/home/lth/chat_server/backups/20260921-files`。

`Output/deployment/chatserver.service` 仍指向较早的 storage 版本，`README.md` 也包含早期记录。**不要用它们直接覆盖当前线上单元。** `deploy-files.sh` 是一次性的 003 迁移部署脚本，有固定版本、哈希和“Media.kind 不存在”前置条件，不能直接重复运行。

## 5. 配置契约

此前线上单元关键配置如下，切换时保留远端实际配置，只改明确需要改的字段：

```ini
[Unit]
Description=ChatServer TCP service
After=network.target mysql.service redis-server.service
Requires=mysql.service redis-server.service

[Service]
Type=simple
User=lth
Group=lth
WorkingDirectory=/home/lth/chat_server/ChatServer
Environment=CHAT_MEDIA_ROOT=/home/lth/chat-media
Environment=CHAT_MEDIA_PORT=6002
Environment=CHAT_MEDIA_URL=http://127.0.0.1:16001
ExecStart=/home/lth/chat_server/releases/20260921-files/ChatServer 0.0.0.0 6000
Restart=on-failure
RestartSec=3
KillSignal=SIGINT
TimeoutStopSec=20
UMask=0027
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
```

当前源码支持 `CHAT_DB_USER`、`CHAT_DB_PASSWORD`、`CHAT_DB_NAME` 覆盖，Redis 端口使用 `CHAT_REDIS_PORT`。数据库主机/端口以及 Redis 主机还应核对新代码；不要假设支持未实现的环境变量。

现有 main.cpp 含数据库连接默认值，文档不抄录其中密码；新服务端若取消默认值，需要先补充真实配置，可在服务器私有 EnvironmentFile 中设置并由 systemd 引用，不把占位密码投入生产。保持工作目录是因为程序可能依赖相对资源路径。

`CHAT_MEDIA_PORT=6002` 是服务端内部监听端口，`CHAT_MEDIA_URL` 是对客户端公布的访问基地址，程序会追加 `/media`。上面单元展示的是原部署快照；直连配置通过 `/etc/systemd/system/chatserver.service.d/media-url.conf` 覆盖为 `http://39.105.18.142`，不要把 `/media` 再写进服务端基地址。客户端的同名环境变量用于 URL 白名单，需要包含完整 `/media` 路径。

## 6. 上传到独立发布目录

以下 PowerShell 块生成本次唯一发布编号，并依次检查命令返回值：

```powershell
$releaseId = Get-Date -Format 'yyyyMMdd-HHmmss'
$remoteRelease = "/home/lth/chat_server/releases/$releaseId"
$artifact = 'D:\chat\_server\Output\linux-x64\Release\ChatServer'
$expectedSha = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
ssh lth@39.105.18.142 "mkdir -m 750 '$remoteRelease'"
if ($LASTEXITCODE -ne 0) { throw '创建发布目录失败' }
scp $artifact "lth@39.105.18.142:$remoteRelease/ChatServer"
if ($LASTEXITCODE -ne 0) { throw '上传失败' }
ssh lth@39.105.18.142 "chmod 755 '$remoteRelease/ChatServer' && sha256sum '$remoteRelease/ChatServer' && file '$remoteRelease/ChatServer' && ldd '$remoteRelease/ChatServer'"
if ($LASTEXITCODE -ne 0) { throw '远端预检失败' }
Write-Output "releaseId=$releaseId expectedSha=$expectedSha"
```

比较远端 SHA256 与 `$expectedSha` 必须完全一致；检查 `ldd` 输出不能有 `not found`。即使 ldd 返回 0，也仍要读输出。若出现 GLIBC/GLIBCXX 版本要求不满足，回到兼容构建环境处理，不在运行前忽略它。

有新 SQL 迁移或额外配置/资源时，也上传到这个发布目录；不要自动再执行 001/002/003。新服务端如果改变数据库结构或启动参数，必须先审查其迁移及运行契约，下面的通用切换不能替代审查。

## 7. 备份和切换

远端执行 `sudo -i` 进入 root shell，使用 bash。下面将发布编号和哈希替换为第 6 节真实结果，不能照抄占位值：

```bash
set -euo pipefail
release_id='替换为本次发布编号'
expected_sha='替换为本次SHA256'
release="/home/lth/chat_server/releases/$release_id"
backup="/home/lth/chat_server/backups/$release_id"
test -f "$release/ChatServer"
test "$(sha256sum "$release/ChatServer" | cut -d ' ' -f 1)" = "$expected_sha"
test ! -e "$backup"
install -d -m 700 "$backup"
cp -a /etc/systemd/system/chatserver.service "$backup/chatserver.service"
systemctl cat chatserver.service > "$backup/effective-service.txt"
```

若有效单元有 drop-in 或 EnvironmentFile，先将其也备份，并确认没有 drop-in 覆盖 ExecStart。以下替换方式适用于此前的单一服务文件布局。

```bash
systemctl stop chatserver.service
test "$(systemctl show chatserver.service -p MainPID --value)" = 0
umask 077
mysqldump --single-transaction --routines --events --triggers --hex-blob --no-tablespaces chat > "$backup/chat.sql"
tar -czf "$backup/media.tar.gz" -C /home/lth chat-media
test -s "$backup/chat.sql"
gzip -t "$backup/media.tar.gz"
mysql -NBe 'SELECT COUNT(*) FROM chat.History; SELECT COUNT(*) FROM chat.Media' > "$backup/counts-before.txt"
sha256sum "$backup/chat.sql" "$backup/media.tar.gz" > "$backup/SHA256SUMS"
```

此前还执行了备份恢复验证；复用该步骤时创建独立临时数据库，验证完仅删除该临时库：

```bash
verify="chat_restore_${release_id//-/_}"
mysql -e "CREATE DATABASE $verify CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
mysql "$verify" < "$backup/chat.sql"
mysql -NBe "SELECT COUNT(*) FROM $verify.History; SELECT COUNT(*) FROM $verify.Media" > "$backup/counts-restored.txt"
cmp "$backup/counts-before.txt" "$backup/counts-restored.txt"
mysql -e "DROP DATABASE $verify"
```

此时才执行本次经过审查且尚未应用的迁移，例如 `mysql chat < "$release/实际迁移文件.sql"`。没有迁移就跳过。MySQL DDL 可能分步提交，失败后需检查实际结构，不能认为整个迁移自动回滚。备份或迁移失败时不要继续启动新程序。

确认新程序仍以 `ChatServer 0.0.0.0 6000` 启动后切换：

```bash
sed "s@^ExecStart=.*@ExecStart=$release/ChatServer 0.0.0.0 6000@" "$backup/chatserver.service" > /etc/systemd/system/chatserver.service
systemctl daemon-reload
systemctl start chatserver.service
systemctl is-active --quiet chatserver.service
systemctl is-enabled chatserver.service
```

不运行第二份手工 ChatServer 占用 6000。此前服务已启用开机启动；若读取结果确实未启用且部署要求开机启动，再执行 `systemctl enable chatserver.service`。

## 8. 验收与回退边界

远端 root shell：

```bash
systemctl show chatserver.service -p ActiveState -p SubState -p MainPID -p NRestarts -p MemoryCurrent
readlink -f /proc/$(systemctl show chatserver.service -p MainPID --value)/exe
sha256sum /proc/$(systemctl show chatserver.service -p MainPID --value)/exe
ss -ltn '( sport = :6000 or sport = :6002 )'
journalctl -u chatserver.service --since '5 minutes ago' --no-pager -n 60
mysql -NBe 'SELECT COUNT(*) FROM chat.History; SELECT COUNT(*) FROM chat.Media'
```

确认 active/running、进程路径和哈希正确、端口正确，启动日志中的 MySQL/Redis 正常。稍后再次查看 NRestarts，排除反复崩溃重启；不能只凭 start 返回成功宣布完成。

业务验证用专门测试账号，验证新 Qt 客户端登录、收发、历史及媒体访问；不要用真实用户账号发送测试消息。匿名 TCP 请求被拒绝、无媒体凭证请求被拒绝，只能证明入口与鉴权基本可达，不能代替完整业务验证。此前媒体未授权 HTTP 返回 400 JSON unauthorized，不要武断断言必须为 401。

若只换二进制、数据库结构与旧程序兼容，可恢复本次备份的 service 文件，daemon-reload 后启动，回到旧版本目录。若数据库已经迁移且不兼容，不能只回退 EXE/ELF；需评估一致恢复数据库、媒体与配置，以及切换后新增数据，不能自动覆盖生产数据。

## 9. 客户端连接：公网直连

2026-10-03 已完成切换：云安全组放行 TCP 80，chatserver 重启后从 drop-in 读取公网媒体基地址。关闭本地 SSH 隧道并确认 16000/16001 无监听后，真实 Qt 客户端视频收发、下载校验、画面播放、暂停 seek、历史与文字消息验证通过；Nginx 对应 PUT/GET 返回 200。切换后 chatserver active/running、PID 74471、NRestarts 0，PID 仅为当时记录，后续检查以实际值为准。

用户已明确选择公网 IP + HTTP 作为当前学习联调方式，客户端不依赖 SSH 隧道。此模式下聊天和文件数据不做传输加密。

- 聊天：`39.105.18.142:7000`，沿用 Nginx 的 TCP 转发入口。
- 文件：`http://39.105.18.142/media`，Nginx 反向代理到 `127.0.0.1:6002`。
- 云安全组必须允许入方向 TCP 7000 和 80；只有服务器本机 curl 成功不能证明公网可达。
- Qt 直接打开客户端 EXE，Electron 直接运行 `npm run dev`。
- 客户端可用 `CHAT_HOST`、`CHAT_PORT` 覆盖聊天入口；客户端 `CHAT_MEDIA_URL` 指定允许的完整公网 HTTP 文件 URL，默认 `http://39.105.18.142/media`。
- 媒体服务继续校验登录会话、会话成员和短期 Bearer 凭证，不把媒体文件夹作为静态目录开放。

对应本仓库配置：

- `deploy/nginx-chat-media.conf` → `/etc/nginx/conf.d/chat-media.conf`。
- `deploy/chatserver-media-url.conf` → `/etc/systemd/system/chatserver.service.d/media-url.conf`。

Nginx 配置启用前执行 `nginx -t`，通过后 reload。systemd drop-in 调整后执行 daemon-reload，并重启 chatserver 让进程读取新的媒体基地址；现有聊天连接会断开，客户端需重新登录。此项改动不需要编译服务端，也不执行数据库迁移。

本次配置备份目录：`/home/lth/chat_server/backups/direct-http-1790993472233`。该次新增的两份配置原本不存在；需要回退时先核对当前文件确属本次配置，将它们移入备份目录，验证 Nginx 后 reload，再 daemon-reload/restart chatserver。回退会恢复原服务单元中的回环转发地址，客户端连接配置也要对应调整。

## 10. 交接执行原则

先读新服务端代码和服务器现状，再复用上述环境。构建目标、依赖、运行参数、数据库迁移、客户端协议必须对应同一版本。同步使用当前工作区源码，不执行 Git 写操作。不要重用历史发布目录或硬编码旧产物哈希。

交付时报告：源码位置、构建命令/耗时/结果、产物哈希、发布和备份目录、迁移情况、实际监听端口、服务运行状态，以及 Qt 客户端如何连接。


## 11. 第七阶段 Range 点播发布记录（2026-10-03）

- 本机构建：`wsl -d ChatUbuntu -u lth --exec bash /mnt/f/linux/_environment/build-chat.sh server Release`，ChatServer / Release / Linux x86_64，Full build passed，94.60 秒。
- 产物：`D:/chat/_server/Output/linux-x64/Release/ChatServer`，56,107,496 字节，SHA-256 `52b469e91a3d452294d5a307e5254269f74daf604ad1fdcc99f231eb684808c9`。
- 运行版本：`/home/lth/chat_server/releases/20261003-135708-range/ChatServer`。
- 发布前备份：`/home/lth/chat_server/backups/20261003-135708-range`，包含服务配置、drop-in、数据库和媒体文件；没有数据库迁移。
- 部署后及真实联调后均为 active/running，PID 75156、NRestarts 0；后一次服务内存约 25 MiB。监听 6000、回环 6002，公网入口仍是 7000/80。以上 PID、内存仅为检查时的快照。
- Nginx 实际 GET /media 返回 206，完整下载仍返回 200；私聊测试账号经过公网完成在线/下载播放、跳转、缓存复用及五分钟凭证刷新。没有在远端编译。
- 对应 Qt 产物：`E:/chat_server_qt/ChatClient/build/Desktop_Qt_5_15_2_MinGW_64_bit-Debug/debug/ChatClient.exe`；详细验证见 `docs/STAGE7_DELIVERY.md`。

## 12. 第八阶段 HLS 点播发布记录（2026-10-03）

当次发布为 `/home/lth/chat_server/releases/20261003-210856-hls/ChatServer`，备份在 `/home/lth/chat_server/backups/20261003-210856-hls`。本机 WSL Release/x86_64 构建通过，96.26 秒；ELF 60,959,120 字节，SHA-256 `6c20fc642d344e090b17dac5064b7be292e855fa7ad023cfa890f0bd0dd5484a`。当次没有数据库迁移。

Nginx `/etc/nginx/conf.d/chat-media.conf` 同时代理 `/media` 与 `/media/hls/...`，后端仍是回环 6002；没有公开存储目录。服务端用同一短期 HLS 凭证逐次校验 master、媒体列表、TS、init.mp4、m4s，原件下载凭证仍只读 `/media`。C++ 新增 `hls_catalog.cpp`；全新同步后若已有 CMake 缓存未重新收集源文件，需要对 `/opt/chat-build/source` 的既有构建目录运行一次配置。

当次 HLS 产物在本机 `lessons/media_process` 生成，由 `publish_hls.py` 校验原件后上传到 `objects/<media_id>/hls/<revision>`，最后原子替换 `catalog`。自动上传后处理已接入第 13 节的部署；云端运行转码，本机负责 Linux 编译。所有列表和子资源继续鉴权。

公网 Qt 的 TS/fMP4 起播、暂停定位、动态档位、切档、客户端续签保留档位通过。验证后服务 active/running、PID76495、NRestarts0、约23MiB（快照）。既有两份样本的12条媒体记录已准备HLS，Sintel只有480p/360p，原无声样本为720p/480p/360p。Qt产物路径不变，构建和验证细节在 `docs/STAGE8_DELIVERY.md`。

## 13. 上传后自动处理部署（2026-10-06）

当前服务目录：`/home/lth/chat_server/releases/20261006-115359-media-jobs`；备份同名目录位于 `/home/lth/chat_server/backups`。部署前备份数据库、媒体与服务配置，并在独立临时库核对数据库恢复。已应用 `migrations/004_media_jobs.sql`，创建 MediaJob 并为可用既有 MP4 补任务。

上传 MP4 原件就绪时在同一事务入队，服务后台启动 Linux `media_process` 转码和分片。配置位于 `/etc/systemd/system/chatserver.service.d/media-processing.conf`：ExecStart 指向该发布目录的 ChatServer，`CHAT_MEDIA_PROCESS` 指向同目录 media_process，`CHAT_HLS_SEGMENT_TYPE=ts`。同机单个任务、各档串行，云端只运行不编译。

两个 Linux 目标可用本机 WSL 的 `deploy/build-media-server.sh` 编译，产物在 `D:/chat/_server/Output/linux-x64/Release`。`deploy/install-media-server.sh` 执行已审查的首次任务表迁移与发布，不适合不加核对地重复运行。后续部署先检查表、drop-in 和实际运行路径，不能仅改主 service 而忽略 ExecStart 被 drop-in 覆盖。

两份真实公网新上传视频已自动生成 HLS，Qt 实际首帧、暂停定位、切档、续签通过；处理中可播放原件，完成后重新打开取得清晰度。运行快照 active/running、PID87336、NRestarts0，服务及子进程内存峰值约257MiB。入口仍为7000/80，后端6000/回环6002。

完整构建命令、产物摘要、验证证据和回退边界见 `docs/STAGE8_AUTOMATIC_VOD_DELIVERY.md`。回退前恢复并核对 service 和 drop-in；新增任务表不自动删除，更不能覆盖上线后业务数据。
