# Qt 聊天客户端

本目录包含 `ChatClient` 的 `future_rtp` 分支源码，对应提交 `7b14b8a100e609da3e58224496cc913159ddb8d0`。支持聊天、MP4 上传与下载、HTTP/HLS 点播、RTMP 桌面直播和 WebRTC 摄像头直播。

## 工程与依赖

入口为 `ChatClient.pro`，使用 qmake 独立构建，不加入服务端的 CMake 构建。桌面和麦克风采集使用 Windows API，当前目标平台为 Windows x64。

| 项目 | 配置 |
| --- | --- |
| Qt / 编译器 | Qt 5.15.2、MinGW 8.1 x64，C++17 |
| FFmpeg | 8.1.1 shared 开发包，含头文件、导入库和运行时 DLL |
| RTC | libdatachannel 0.24.6、Mbed TLS 3.6.4 及其依赖，使用同一 MinGW 工具链编译 |
| `FFMPEG_ROOT` | FFmpeg 开发包根目录；默认 `D:/chat/_deps/ffmpeg-8.1.1-full_build-shared` |
| `RTC_ROOT` | RTC 源码与静态库目录；默认 `D:/chat/_deps/rtc` |

依赖库不随源码目录复制。RTC 的版本清单、目录布局和构建命令见 [rtc/README.md](rtc/README.md) 与 `rtc/dependency-sources.json`。通过环境变量或 qmake 参数设置 `FFMPEG_ROOT`、`RTC_ROOT`，即可使用其他位置的依赖。

## 构建与打包

在 Qt Creator 中打开 `ChatClient.pro`，选择上述 Qt/MinGW Kit 并配置依赖路径即可。使用独立构建目录；Qt Creator 会重新生成本机的 `.pro.user`，该文件不提交。

也可在普通 CMD 中执行以下命令。Qt 与依赖路径为本机示例，其他电脑按安装位置调整：

```bat
cd /d D:\chat_server\ChatServer\fronted_qt
set "FFMPEG_ROOT=D:\chat\_deps\ffmpeg-8.1.1-full_build-shared"
set "RTC_ROOT=D:\chat\_deps\rtc"
set "PATH=F:\Qt\Tools\mingw810_64\bin;F:\Qt\5.15.2\mingw81_64\bin;%PATH%"
if not exist build mkdir build
cd build
qmake -o Makefile ..\ChatClient.pro -spec win32-g++ CONFIG+=release CONFIG-=debug
mingw32-make -j2 release
windeployqt --release --compiler-runtime release\ChatClient.exe
copy /Y "%FFMPEG_ROOT%\bin\*.dll" release\
```

成品目录为 `build/release/`。分发时保留整个目录中的 Qt 插件、Qt/FFmpeg DLL 和编译器运行库，不能只发送 EXE。这里只保留主程序源码、资源及依赖说明，不包含 Electron、独立测试程序或已有构建产物。

## 模块

- 根目录：登录、聊天、文件传输、直播卡片与授权请求。
- `player/`：文件/HTTP/HLS 输入，共用解码、音视频时钟和显示。
- `broadcast/`：桌面、电脑声音与麦克风采集，H.264/AAC 编码和 RTMP 推流。
- `rtc/`：摄像头直播、H.264/Opus 编码、WebRTC 信令及 RTP/RTCP 收发。
- `resourse.qrc` 与根目录图片：Qt 界面资源，构建时打入程序。

## 连接服务端

默认聊天入口为 `39.105.18.142:7000`，文件入口为 `http://39.105.18.142/media`。可用 `CHAT_HOST`、`CHAT_PORT`、`CHAT_MEDIA_URL` 指定环境；直播对应 `CHAT_LIVE_URL`、`CHAT_LIVE_RTMP_URL` 和 `CHAT_LIVE_RTC_URL`。

摄像头直播要求服务端包含 `future_rtp` 中的 RTC 信令、数据库迁移、SRS 配置及 RTCP SR 补丁。将本目录放入后端仓库不会自动补齐后端功能；服务端原有 `main` 分支不包含这些 RTC 改动。
