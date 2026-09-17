# 图片处理与磁盘存储核心

这部分可独立编译，不依赖聊天服务、MySQL、Redis 或 Electron。服务端使用库；Electron 主进程可以启动 `chat_image` 子进程。图片消息、HTTP 上传、数据库关联尚由后续集成实现，当前工具本身不发送消息。

## 构建和测试

依赖 C++20、CMake >= 3.16、OpenCV 4 的 core/imgproc/imgcodecs 模块。启用测试时还需要 Python 3；测试只使用 Python 标准库。

在仓库根目录执行：

```sh
cmake -S media -B media/build/linux -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build media/build/linux --target chat_image image_test disk_store_test --parallel 2
ctest --test-dir media/build/linux --output-on-failure
```

Windows 使用 Visual Studio x64 生成器并通过 `OpenCV_DIR` 指向已安装库的 CMake 配置目录，构建指定 `--config Release`，CTest 指定 `-C Release`。运行前把 OpenCV DLL 目录加入当前进程 PATH。项目不会下载依赖、修改系统 PATH 或部署程序。

本机验证使用 MSVC 19.44、OpenCV 4.13.0；远程已只读核实为 Ubuntu 24.04、GCC 13.3、CMake 3.28.3、OpenCV 4.6.0。远程执行结果单独记录，不以本机结果替代。

## 工具契约

```sh
chat_image /path/to/input.png /private/cache/jobs/job-id thumbnail
```

三个参数依次为输入文件、独立输出目录、对象 key。文件写入 `<输出目录>/objects/<key>`；现有对象不会被覆盖。Windows 入口使用宽字符参数，支持中文文件路径。

成功时退出码为 0，stdout 恰好一行 JSON：

```json
{"source":{"width":640,"height":320,"mime":"image/png"},"thumbnail":{"key":"thumbnail","width":320,"height":160,"mime":"image/png","bytes":687}}
```

`bytes` 是实际编码字节数，上例仅展示结构。失败时退出码为 1，stdout 返回 `{"error":"invalid_image"}` 等错误码。stderr 供编解码库诊断；调用方分别收集两个流，限制日志长度，不把 stderr 当作 JSON。

错误码包括 `invalid_argument`、`io_error`、`invalid_image`、`unsupported_format`、`byte_limit` 和 `pixel_limit`。失败结果不代表可以无条件重试：错误参数/不支持的格式需让用户修改输入；IO 错误需先检查目录、空间、重复 key 或文件访问权限。

## 处理边界

- 默认接受静态 JPEG/PNG，限制输入 20 MiB、2400 万像素、单边 16000 像素。头部尺寸在 OpenCV 解码前检查；检查不是完整的恶意文件证明。
- 缩略图最长边 320，不放大小图；JPEG 应用 EXIF 方向。透明输出使用 PNG，其余缩略图使用 JPEG；16 位 PNG 缩略图降至 8 位。输入文件不被重写。
- 核心按实际编码识别格式，拒绝 APNG。当前不做色彩管理、动图或视频处理。
- 每个调用会持有压缩输入、解码像素和缩放结果。24MP RGBA16 的解码矩阵约 192 MB，不能将 20 MiB 输入限制视为内存上限；上层需要有限并发、队列和进程内存/超时约束。
- OpenCV `IMREAD_UNCHANGED` 与 EXIF 方向处理的关系见[官方文档](https://docs.opencv.org/4.13.0/d4/da8/group__imgcodecs.html)。处理 JPEG 和透明 PNG 时选择不同解码标志是有意的。

## 磁盘对象边界

`DiskStore::Begin` 声明准确字节数，`Append` 可分块写入，`Commit` 只有在字节数吻合时发布。上传对象析构、超限或错误会关闭并清理暂存文件。

发布使用同一文件系统上的硬链接，目标已存在时失败，避免两个写入者互相覆盖。文件系统需支持硬链接（例如 ext4、NTFS）；暂存与对象目录不能跨卷。该实现不适用于任意网络文件系统的持久性假设。

key 仅允许小写字母、数字、`-`、`_` 和分隔用 `/`，不能含点、反斜杠、空路径段或绝对路径。key 由业务服务生成，不使用用户提供的原文件名。路径检查拒绝符号链接；存储根目录应由服务账户独占，不能让不可信本地进程并发替换路径。

写入提交会同步文件，Linux 还同步目标父目录。Windows 目录元数据断电持久性未作保证；这些步骤不构成数据库与文件的原子事务。进程被强制终止可能留下暂存目录，上层恢复任务需在排除活跃上传后回收。

## 阅读顺序

1. `include/chat/media/image.h`：输入限额与结果契约。
2. `tests/image_test.cpp` 与 `src/image.cpp`：先看真实输入和断言，再看头部检查、解码、方向、缩放与编码。
3. `tests/disk_store_test.cpp` 与 `src/disk_store.cpp`：理解暂存所有权、错误清理和无覆盖发布。
4. `tests/image_tool_test.py` 与 `src/image_tool.cpp`：理解文件路径、子进程退出码与结构化输出边界。
