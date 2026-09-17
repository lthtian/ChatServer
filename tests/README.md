# 媒体数据库测试

这些测试使用真实 MySQL，不需要启动 ChatServer 或连接业务数据库。已按服务器 MySQL 8.0.46 的表结构建立最小夹具，测试数据库使用随机名称，清理只作用于本次创建的数据库和账户。

```sh
cmake -S tests -B tests/build -DCMAKE_BUILD_TYPE=Release
cmake --build tests/build --target media_repository_test --parallel 1
sudo -v
python3 tests/media_schema_test.py --sudo --repository-test tests/build/media_repository_test -v
```

依赖 GCC 的 C++20 协程支持、Boost >= 1.82、OpenSSL、MySQL 客户端和 Python 3。不需要 OpenCV 链接库；仓储只保存已经处理好的图片元数据。1.6 GiB 服务器曾在单任务 Release 编译时发生严重内存压力，日常构建使用本地 WSL2，不能仅靠降低并发控制单个编译单元的峰值。

Python 脚本先创建随机 `chat_media_test_*` 数据库和仅能读写该库的 `chat_test_*` 用户，再执行 C++ 场景。密码只通过子进程环境传递，命令行和输出不包含连接凭据。管理员连接仅负责夹具、迁移和清理。当前认证夹具针对 MySQL 8.0 的 `mysql_native_password`，不能直接当成其他数据库版本的认证配置。

省略 `--repository-test` 可单独检查迁移；省略 `--sudo` 时使用当前用户的 MySQL 客户端连接配置。不要把测试程序指向业务库。

本机 WSL 使用 `F:\linux\_environment\build-chat.ps1 -Target repository` 编译。冷启动时先等待 MySQL 服务就绪，再执行测试：

```powershell
& 'C:\Program Files\WSL\wsl.exe' -d ChatUbuntu -u root --exec bash -lc 'set -e; systemctl start mysql; python3 /opt/chat-build/source/tests/media_schema_test.py --repository-test /opt/chat-build/build/tests-Release/media_repository_test -v'
```

阅读顺序：先看 `media_schema_test.py` 中的真实约束断言，再看 `media_repository_test.cpp` 的行为场景，最后对照 `src/server/db/media_repository.cpp` 的权限检查、事务边界与结果映射。
