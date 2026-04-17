# Muduo → Boost.Asio 协程迁移方案

> 目标：将网络层从 muduo 替换为 Boost.Asio + C++20 协程，提升性能、学习现代 C++ 异步编程。
> 策略：分 4 个阶段渐进替换，每个阶段可独立编译运行。

---

## 当前架构依赖分析

### muduo 使用清单

| 组件 | 文件 | 用途 |
|------|------|------|
| `muduo::net::TcpServer` | chatserver.hpp/cpp | TCP 服务器主体 |
| `muduo::net::EventLoop` | main.cpp, chatserver.hpp | 事件循环 |
| `muduo::net::TcpConnectionPtr` | chatservice.hpp/cpp 所有 handler | 连接指针，贯穿业务层 |
| `muduo::net::Buffer` | chatserver.cpp onMessage | 读取缓冲区 |
| `muduo::net::InetAddress` | main.cpp | 监听地址 |
| `muduo::base::Timestamp` | MsgHandler 签名 | 时间戳 |
| `muduo::base::Logging` | chatservice.cpp, db.cpp, connectionpool.cpp | LOG_INFO/LOG_ERROR 等 |

### 架构层次

```
main.cpp (EventLoop)
  └─ ChatServer (TcpServer, 2 IO线程)
       ├─ onConnection → ChatService::clientCloseException
       ├─ onMessage → JSON解析 → TaskQueue → WorkerThreadPool(N线程)
       │                                     └─ ChatService::handler()
       └─ _recvBuffers (连接级缓冲区)
```

---

## Phase 1：网络层替换（保留同步业务层）

### 目标

将 muduo::net 替换为 Boost.Asio，使用 C++20 协程处理连接和读写。
业务层（ChatService）签名保持回调风格不变，仅做类型替换。

### 改动范围

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `CMakeLists.txt`（根目录） | 修改 | 添加 Boost 依赖，移除 muduo |
| `src/server/CMakeLists.txt` | 修改 | 链接 Boost 库替换 muduo |
| `include/server/chatserver.hpp` | **重写** | 整个文件重构为 Session + Server |
| `src/server/chatserver.cpp` | **重写** | Session 协程实现 |
| `src/server/main.cpp` | **重写** | io_context 替代 EventLoop |
| `include/server/chatservice.hpp` | 修改 | 类型替换：TcpConnectionPtr → SessionPtr |
| `src/server/chatservice.cpp` | 修改 | 类型替换，LOG 宏替换 |
| `src/server/db/db.cpp` | 修改 | LOG 宏替换 |
| `src/server/db/connectionpool.cpp` | 修改 | LOG 宏替换 |

### 新增文件

| 文件 | 说明 |
|------|------|
| `include/server/session.hpp` | Session 类，封装单个连接的读写协程 |
| `include/server/chatserver.hpp` | 重写为 Asio 版 ChatServer |

### 详细设计

#### 1.1 Session 类（替代 onConnection + onMessage）

```cpp
// include/server/session.hpp
#pragma once
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <memory>
#include <string>
#include "json.hpp"
#include "chatservice.hpp"

namespace asio = boost::asio;
using asio::ip::tcp;
using asio::use_awaitable;
using json = nlohmann::json;

class ChatServer;  // 前向声明

class Session : public std::enable_shared_from_this<Session> {
public:
    using Ptr = std::shared_ptr<Session>;

    Session(tcp::socket socket, ChatServer& server);
    ~Session();

    void start();  // 启动读写协程

    // 线程安全的发送方法（替代 muduo 的 conn->send()）
    void send(const std::string& msg);

    // 获取远程地址信息（用于日志）
    std::string remote_endpoint() const;

    tcp::socket& socket() { return socket_; }

private:
    asio::awaitable<void> read_loop();   // 读协程：循环读取 + JSON 解析 + 分发
    void close();                         // 关闭连接，触发 clientCloseException

    tcp::socket socket_;
    ChatServer& server_;
    asio::streambuf buffer_;              // 替代 muduo::net::Buffer
    bool closed_ = false;
};
```

**核心改动点**：

- `muduo::net::Buffer` → `asio::streambuf`
- `onMessage` 的 JSON 大括号匹配逻辑搬到 `read_loop()` 协程内
- `conn->send()` → `session->send()`（内部通过 post 保证线程安全）
- 连接断开检测：协程退出时自动触发 `clientCloseException`

#### 1.2 Session::read_loop() 协程逻辑

```
read_loop() 协程流程：
┌──────────────────────────────────────┐
│ while (socket_.is_open()) {          │
│   co_await async_read_some(buffer)   │ ← 替代 muduo 的 onMessage 回调
│   buffer → string                    │
│   大括号匹配提取完整 JSON（复用现有逻辑）│
│   json::parse(json_str)              │
│   getHandler(msgid)                  │
│   handler(session, js, timestamp)    │ ← 回调风格不变
│ }                                    │
│ // 协程退出 = 连接断开               │
│ ChatService::clientCloseException()  │ ← 替代 onConnection(conn->connected()==false)
└──────────────────────────────────────┘
```

**你现有的 JSON 大括号匹配逻辑原封不动搬入 read_loop()**，只是从回调函数变成了协程内的线性代码。

#### 1.3 Session::send() 线程安全

muduo 的 `TcpConnection::send()` 是线程安全的，asio 的 socket 写操作不是。
必须用 post 序列化写操作：

```cpp
void Session::send(const std::string& msg) {
    auto self = shared_from_this();
    asio::post(socket_.get_executor(), [self, msg]() {
        // 这里在 socket 所属的执行器上运行，保证串行
        if (self->closed_) return;
        asio::async_write(self->socket_, asio::buffer(msg),
            [self](boost::system::error_code ec, size_t) {
                if (ec) self->close();
            });
    });
}
```

或者更简单的方案：用一条 `asio::strand` 包装所有写操作。

#### 1.4 ChatServer 类（替代 muduo::net::TcpServer）

```cpp
// include/server/chatserver.hpp（重写后）
#pragma once
#include <boost/asio.hpp>
#include <string>
#include <memory>
#include "session.hpp"

namespace asio = boost::asio;
using asio::ip::tcp;

class ChatServer {
public:
    ChatServer(asio::io_context& ioc, const std::string& ip, uint16_t port);
    void start();

private:
    asio::awaitable<void> do_accept();  // 接受连接的协程

    asio::io_context& ioc_;
    tcp::acceptor acceptor_;
};
```

```cpp
// chatserver.cpp
ChatServer::ChatServer(asio::io_context& ioc, const std::string& ip, uint16_t port)
    : ioc_(ioc),
      acceptor_(ioc, tcp::endpoint(asio::ip::make_address(ip), port))
{}

void ChatServer::start() {
    asio::co_spawn(ioc_, do_accept(), asio::detached);
}

asio::awaitable<void> ChatServer::do_accept() {
    while (true) {
        auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
        auto session = std::make_shared<Session>(std::move(socket), *this);
        session->start();
    }
}
```

#### 1.5 main.cpp 重写

```cpp
// main.cpp
#include "chatserver.hpp"
#include "chatservice.hpp"
#include <boost/asio.hpp>
#include <iostream>
#include <signal.h>

namespace asio = boost::asio;

asio::io_context* g_ioc = nullptr;

void resetHandler(int) {
    std::cout << "\n[SERVER] Shutting down..." << std::endl;
    ChatService::instance()->reset();
    if (g_ioc) g_ioc->stop();
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s ip port\n", argv[0]);
        return 1;
    }

    signal(SIGINT, resetHandler);
    signal(SIGSEGV, resetHandler);
    signal(SIGABRT, resetHandler);

    asio::io_context ioc(2);  // 2 个线程，等价于之前 setThreadNum(2)
    g_ioc = &ioc;

    ChatServer server(ioc, argv[1], std::atoi(argv[2]));
    server.start();

    // 运行事件循环（等价于 muduo 的 loop.loop()）
    ioc.run();
    return 0;
}
```

#### 1.6 ChatService 类型替换

Phase 1 的核心原则：**handler 签名只做类型替换，不改逻辑**。

```cpp
// 改动前
using MsgHandler = std::function<void(const TcpConnectionPtr &conn, json &js, Timestamp time)>;

// 改动后
using MsgHandler = std::function<void(const Session::Ptr &session, json &js, Timestamp time)>;
```

所有 15 个 handler 函数签名做同样替换：

```
sed -i 's/const TcpConnectionPtr &conn/const Session::Ptr \&session/g'
```

handler 内部的 `conn->send()` → `session->send()`，也是全局替换。

`_userConnMap` 类型：
```cpp
// 改动前
unordered_map<int, TcpConnectionPtr> _userConnMap;

// 改动后
unordered_map<int, Session::Ptr> _userConnMap;
```

#### 1.7 日志替换

| muduo | 替换方案 |
|-------|---------|
| `LOG_INFO << "xxx"` | `std::cout << "[INFO] xxx\n"` 或引入 spdlog |
| `LOG_ERROR << "xxx"` | `std::cerr << "[ERROR] xxx\n"` |
| `LOG_WARN << "xxx"` | `std::cout << "[WARN] xxx\n"` |

推荐：引入 `spdlog`（异步日志，性能好），或者简单起见先换成 `std::cout/cerr` + 格式前缀。

#### 1.8 CMakeLists.txt 修改

**根目录 CMakeLists.txt**：

```cmake
# 添加
find_package(Boost 1.82 REQUIRED COMPONENTS system)
include_directories(${Boost_INCLUDE_DIRS})
link_directories(${Boost_LIBRARY_DIRS})

# 移除
# include_directories(/usr/local/include)  # muduo 头文件路径
# link_directories(/usr/local/lib)         # muduo 库路径
```

**src/server/CMakeLists.txt**：

```cmake
# 改动前
target_link_libraries(ChatServer ${OpenCV_LIBS} muduo_net muduo_base mysqlclient hiredis pthread ssl crypto)

# 改动后
target_link_libraries(ChatServer
    ${OpenCV_LIBS}
    ${Boost_LIBRARIES}
    mysqlclient hiredis pthread ssl crypto
)
```

#### 1.9 _recvBuffers 迁移

当前 `_recvBuffers` 是 `ChatServer` 的成员变量，用 `TcpConnectionPtr` 做 key。
新方案中，每个 `Session` 自带 `asio::streambuf buffer_`，不需要外部的 map。

JSON 大括号匹配逻辑从 `ChatServer::onMessage` 搬到 `Session::read_loop()` 内部，
每次 read 后直接在 Session 本地的 buffer 上做解析。

### Phase 1 验证标准

- [ ] 编译通过，无 muduo 依赖
- [ ] 客户端能正常连接、登录、注册
- [ ] 一对一聊天、群聊功能正常
- [ ] 异常断开能正确触发 clientCloseException
- [ ] 信号处理（Ctrl+C）正常关闭
- [ ] 功能与替换前完全一致

---

## Phase 2：消除手写线程池，handler 协程化

### 目标

去掉 `TaskQueue<Task>` + `WorkerThreadPool`（约 100 行代码），
每个业务 handler 变成 `asio::awaitable<void>` 协程。

### 改动范围

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `include/server/chatserver.hpp` | 修改 | 删除 Task、TaskQueue、WorkerThreadPool |
| `include/server/session.hpp` | 修改 | read_loop 改为 co_await 调用协程 handler |
| `include/server/chatservice.hpp` | 修改 | MsgHandler 签名改为 awaitable |
| `src/server/chatservice.cpp` | **重写** | 所有 handler 变成协程 |

### 详细设计

#### 2.1 新的 MsgHandler 签名

```cpp
// Phase 1 的签名（回调风格）
using MsgHandler = std::function<void(const Session::Ptr&, json&, Timestamp)>;

// Phase 2 的签名（协程风格）
using MsgHandler = std::function<asio::awaitable<void>(const Session::Ptr&, json&)>;
```

注意：去掉了 `Timestamp` 参数（实际未使用，可用 `std::chrono` 替代）。

#### 2.2 Session::read_loop 改动

```cpp
// Phase 1（回调分发）
auto handler = ChatService::instance()->getHandler(msgid);
handler(shared_from_this(), js, timestamp);

// Phase 2（协程分发）
auto handler = ChatService::instance()->getHandler(msgid);
co_await handler(shared_from_this(), js);
```

#### 2.3 handler 协程化示例 — login

```cpp
// Phase 1（回调）
void ChatService::login(const Session::Ptr& session, json& js, Timestamp time) {
    User user = _userModel.query(name);  // 阻塞查询
    // ... 处理逻辑 ...
    session->send(response.dump());
}

// Phase 2（协程）
asio::awaitable<void> ChatService::login(const Session::Ptr& session, json& js) {
    auto executor = co_await asio::this_coro::executor;

    // 将阻塞的 DB 操作 post 到全局线程池执行器
    std::string name = js["username"];
    std::string password = js["password"];

    User user = co_await asio::post(
        executor,
        asio::use_awaitable,
        [=]() { return _userModel.query(name); }
    );

    if (user.getName() == name && user.getPwd() == password) {
        if (user.getState() == "online") {
            json response;
            response["msgid"] = LoginMsgAck;
            response["errmsg"] = "该用户已经登录";
            response["errno"] = 1;
            session->send(response.dump());
            co_return;
        }

        user.setState("online");
        co_await asio::post(executor, asio::use_awaitable,
            [=]() { _userModel.updateState(user); });

        {
            lock_guard<mutex> lock(_connMutex);
            _userConnMap.insert({user.getId(), session});
        }

        _redis.subscribe(user.getId());

        json response;
        response["msgid"] = LoginMsgAck;
        response["errno"] = 0;
        response["id"] = user.getId();
        response["name"] = user.getName();
        session->send(response.dump());
    } else {
        json response;
        response["msgid"] = LoginMsgAck;
        response["errno"] = 2;
        response["errmsg"] = "用户不存在或密码错误";
        session->send(response.dump());
    }
}
```

#### 2.4 阻塞操作的协程包装策略

所有 Model 层的 DB 调用都是同步阻塞的。Phase 2 的策略是 **post 到 asio 线程池执行器**：

```cpp
// 封装一个辅助方法
template<typename Func>
auto async_db_call(Func&& func) -> asio::awaitable<decltype(func())> {
    auto executor = co_await asio::this_coro::executor;
    co_return co_await asio::post(
        executor,
        asio::use_awaitable,
        std::forward<Func>(func));
}
```

这样所有阻塞 DB 调用都不直接占用 IO 线程：

```cpp
auto user = co_await async_db_call([&]{ return _userModel.query(name); });
auto friends = co_await async_db_call([&]{ return _friendModel.query(id); });
```

#### 2.5 删除的代码

Phase 2 完成后，以下代码可以完全删除：

- `struct Task`（chatserver.hpp 中的任务结构体）
- `class TaskQueue<T>`（chatserver.hpp 中的任务队列模板）
- `class WorkerThreadPool`（chatserver.hpp 中的业务线程池）
- `ChatServer::_taskQueue` 成员变量
- `ChatServer::_workerPool` 成员变量
- `chatserver.cpp` 中构造函数里创建线程池的代码

### Phase 2 验证标准

- [ ] 编译通过
- [ ] 所有功能与 Phase 1 一致
- [ ] TaskQueue、WorkerThreadPool 代码已删除
- [ ] handler 代码是线性的协程风格，无回调嵌套
- [ ] 性能不下降（DB 操作不阻塞 IO 线程）

---

## Phase 3：数据库层异步化

### 目标

将 MySQL 从同步阻塞驱动替换为 `boost::mysql`（Asio 原生异步 MySQL 驱动），
实现完全非阻塞的数据库操作。

### 前提条件

- `boost::mysql` 需要 Boost 1.82+
- MySQL 8.0+ 服务端（或兼容的 MariaDB）
- Phase 2 已完成，所有 handler 都是协程

### 改动范围

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `include/server/db/db.h` | **重写** | MySQL 类改为 async_mysql 封装 |
| `src/server/db/db.cpp` | **重写** | 所有方法变为 awaitable |
| `include/server/db/connectionpool.h` | **重写** | 连接池变为异步获取 |
| `src/server/db/connectionpool.cpp` | **重写** | 异步连接池实现 |
| `include/server/model/*.hpp` | 修改 | DAO 方法签名变为 awaitable |
| `src/server/model/*.cpp`（如果有） | 修改 | 实现改为 co_await 调用 |

### 详细设计

#### 3.1 boost::mysql 基本用法

```cpp
#include <boost/mysql.hpp>
namespace mysql = boost::mysql;

// 异步连接
asio::awaitable<void> connect_example() {
    auto executor = co_await asio::this_coro::executor;
    mysql::tcp_connection conn(executor);

    mysql::handshake_params params("user", "password", "database");
    auto endpoint = tcp::endpoint(asio::ip::make_address("127.0.0.1"), 3306);

    co_await conn.async_connect(endpoint, params, asio::use_awaitable);

    // 异步查询
    auto result = co_await conn.async_execute(
        "SELECT id, name FROM User WHERE name='test'",
        asio::use_awaitable);

    // 读取行
    mysql::rows rows = co_await result.async_read_some(asio::use_awaitable);
    for (const auto& row : rows) {
        int id = row.at(0).as_int64();
        std::string name = row.at(1).as_string();
    }

    co_await conn.async_close(asio::use_awaitable);
}
```

#### 3.2 AsyncMySQL 连接封装

```cpp
// include/server/db/async_db.h
class AsyncMySQL {
public:
    AsyncMySQL(asio::any_io_executor executor);

    asio::awaitable<void> connect(const std::string& host,
                                   const std::string& user,
                                   const std::string& password,
                                   const std::string& database,
                                   uint16_t port);

    asio::awaitable<mysql::rows> query(const std::string& sql);
    asio::awaitable<void> execute(const std::string& sql);
    asio::awaitable<int64_t> last_insert_id();

    asio::awaitable<void> close();
    bool is_connected() const;

private:
    mysql::tcp_connection conn_;
    bool connected_ = false;
};
```

#### 3.3 异步连接池

```cpp
// include/server/db/async_connectionpool.h
class AsyncConnectionPool {
public:
    static AsyncConnectionPool* instance();

    asio::awaitable<void> init(asio::any_io_executor executor,
                                const DBConfig& config,
                                int pool_size);

    // 异步获取连接（无可用连接时挂起协程而非阻塞线程）
    asio::awaitable<std::shared_ptr<AsyncMySQL>> get_connection();

    // 归还连接
    void return_connection(std::shared_ptr<AsyncMySQL> conn);

private:
    std::queue<std::shared_ptr<AsyncMySQL>> pool_;
    std::mutex mutex_;
    asio::steady_timer wait_timer_;  // 用于挂起等待的协程
    // ...
};
```

**关键区别**：Phase 2 的连接池是 `condition_variable::wait()` 阻塞线程；
Phase 3 是 `co_await timer` 挂起协程，不消耗线程资源。

#### 3.4 RAII Guard 保持兼容

```cpp
// 继续使用 RAII 模式，但现在是协程友好的
class AsyncConnectionGuard {
public:
    AsyncConnectionGuard() = delete;
    // 必须在协程中构造
    static asio::awaitable<std::unique_ptr<AsyncConnectionGuard>> create();

    ~AsyncConnectionGuard();
    AsyncMySQL* get();
private:
    AsyncConnectionGuard(std::shared_ptr<AsyncMySQL> conn);
    std::shared_ptr<AsyncMySQL> conn_;
};

// 使用方式
auto guard = co_await AsyncConnectionGuard::create();
auto rows = co_await guard->get()->query("SELECT ...");
```

#### 3.5 Model 层改造示例

```cpp
// usermodel.hpp — 改动前
class UserModel {
    bool insert(User& user);
    User query(const string& name);
    // ...
};

// usermodel.hpp — 改动后
class UserModel {
    asio::awaitable<bool> insert(User& user);
    asio::awaitable<User> query(const string& name);
    asio::awaitable<bool> updateState(const User& user);
    asio::awaitable<string> queryState(int id);
    asio::awaitable<void> resetState();
};
```

```cpp
// usermodel.cpp — 示例实现
asio::awaitable<User> UserModel::query(const string& name) {
    auto guard = co_await AsyncConnectionGuard::create();
    auto& conn = *guard->get();

    // 使用参数化查询防止 SQL 注入
    auto stmt = conn.prepare_statement("SELECT id, name, password, state FROM User WHERE name=?");
    auto result = co_await conn.async_execute_statement(stmt, std::make_tuple(name), asio::use_awaitable);
    auto rows = co_await result.async_read_some(asio::use_awaitable);

    User user;
    if (!rows.empty()) {
        user.setId(rows.at(0).at(0).as_int64());
        user.setName(std::string(rows.at(0).at(1).as_string()));
        user.setPwd(std::string(rows.at(0).at(2).as_string()));
        user.setState(std::string(rows.at(0).at(3).as_string()));
    }
    co_return user;
}
```

#### 3.6 handler 中的变化

```cpp
// Phase 2（post 到线程池执行同步 DB 调用）
auto user = co_await async_db_call([&]{ return _userModel.query(name); });

// Phase 3（直接 co_await 异步 DB 调用）
auto user = co_await _userModel.query(name);
```

handler 代码变得更干净，不需要 lambda 包装了。

#### 3.7 CMakeLists.txt 变更

```cmake
# boost::mysql 在 Boost 1.82+ 中可用
find_package(Boost 1.82 REQUIRED COMPONENTS system)

# 不再需要 mysqlclient C 客户端库
# target_link_libraries 中移除 mysqlclient
```

### Phase 3 验证标准

- [ ] 编译通过，无 mysqlclient C API 依赖
- [ ] 所有功能与 Phase 2 一致
- [ ] DB 查询不再阻塞任何线程
- [ ] 连接池获取不消耗线程（协程挂起）
- [ ] SQL 注入风险消除（参数化查询）
- [ ] 高并发下性能有明显提升

---

## Phase 4：Redis 异步化 + 架构优化

### 目标

1. 将 Redis 从 hiredis 同步阻塞替换为异步操作
2. 用 `asio::strand` 替代 `_connMutex` 锁
3. 优化连接管理架构

### 改动范围

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `include/server/redis/redis.hpp` | **重写** | 异步 Redis 封装 |
| `src/server/redis/redis.cpp` | **重写** | 异步实现 |
| `include/server/chatservice.hpp` | 修改 | _userConnMap 改用 strand 保护 |
| `src/server/chatservice.cpp` | 修改 | 消除手动 mutex |

### 详细设计

#### 4.1 Redis 异步方案选择

有两个方案：

**方案 A：继续用 hiredis + asio 集成**

将 hiredis 的文件描述符注册到 asio 的 io_context 上，实现异步读写。

```cpp
class AsyncRedis {
public:
    AsyncRedis(asio::any_io_executor executor);

    asio::awaitable<bool> connect(const std::string& host, uint16_t port);
    asio::awaitable<bool> publish(int channel, const std::string& message);
    asio::awaitable<void> subscribe(int channel);
    asio::awaitable<void> unsubscribe(int channel);

private:
    // 将 hiredis fd 注册到 asio
    void register_to_asio();
    asio::awaitable<void> subscribe_loop();

    redisAsyncContext* actx_;
    tcp::socket asio_socket_;  // 包装 hiredis fd
};
```

**方案 B：换用 Boost.Redis 或自实现 Redis 协议**

Boost.Redis（Boost 1.87+）是原生 Asio 兼容的 Redis 客户端。

```cpp
#include <boost/redis.hpp>
namespace redis = boost::redis;

class AsyncRedis {
    redis::connection conn_;
    // publish/subscribe 全部 co_await
};
```

**推荐**：如果 Boost 版本够新（1.87+），用方案 B；
否则用方案 A（hiredis + asio 集成，更成熟）。

#### 4.2 subscribe 协程化

当前实现：独立线程中 `while(redisGetReply())` 阻塞循环。

```cpp
// 当前（阻塞线程）
void Redis::observer_channel_message() {
    while (!_stop_flag && REDIS_OK == redisGetReply(...)) {
        _notify_message_handler(channel, message);
    }
}

// Phase 4（协程）
asio::awaitable<void> AsyncRedis::subscribe_loop() {
    while (true) {
        // co_await 等待订阅消息，不阻塞线程
        auto msg = co_await async_read_subscribe_message();
        if (msg) {
            _notify_message_handler(msg->channel, msg->data);
        }
    }
}
```

核心改变：**不再需要一个专门的订阅线程**，subscribe_loop 是一个协程，在没有消息时挂起。

#### 4.3 _userConnMap 去锁

```cpp
// 当前（手动 mutex）
mutex _connMutex;
unordered_map<int, Session::Ptr> _userConnMap;

// Phase 4 方案 A：strand 保护
asio::strand<asio::io_context::executor_type> _mapStrand;

asio::awaitable<void> login(...) {
    // 在 strand 上操作 map，无需 mutex
    co_await asio::post(_mapStrand, asio::use_awaitable);
    _userConnMap[id] = session;
}

// Phase 4 方案 B：保留 mutex（务实选择）
// 对这个规模的项目，mutex 的性能完全够用，strand 反而增加复杂度
// 建议：先保留 mutex，如果 profiling 发现瓶颈再换
```

#### 4.4 连接生命周期管理优化

当前问题：`clientCloseException` 中遍历整个 map 找连接对应的用户。

```cpp
// 当前 O(n) 遍历
for (auto e : _userConnMap) {
    if (e.second == conn) { ... }
}

// 优化：维护反向映射
unordered_map<Session::Ptr, int> _connUserMap;  // 反向查找 O(1)

// Session 断开时
void Session::close() {
    // 协程退出时自动调用
    ChatService::instance()->clientCloseException(shared_from_this());
}
```

### Phase 4 验证标准

- [ ] Redis subscribe 不再使用独立线程
- [ ] publish/subscribe 全部异步
- [ ] 跨服务器消息传递功能正常
- [ ] _userConnMap 操作线程安全
- [ ] 整体性能对比 Phase 3 有提升

---

## 各阶段文件变更汇总

```
Phase 1（网络层替换）:
  重写: chatserver.hpp, chatserver.cpp, main.cpp
  新增: session.hpp
  修改: chatservice.hpp, chatservice.cpp, db.cpp, connectionpool.cpp
  修改: CMakeLists.txt (2个)
  移除: muduo 依赖

Phase 2（消除线程池）:
  修改: chatserver.hpp (删除 Task/TaskQueue/WorkerThreadPool)
  修改: chatservice.hpp, chatservice.cpp (handler 协程化)
  修改: session.hpp (co_await handler)

Phase 3（DB 异步化）:
  重写: db.h, db.cpp, connectionpool.h, connectionpool.cpp
  修改: 所有 model/*.hpp（方法签名 awaitable）
  修改: CMakeLists.txt (mysqlclient → boost::mysql)

Phase 4（Redis 异步化 + 优化）:
  重写: redis.hpp, redis.cpp
  修改: chatservice.hpp/cpp（去锁/反向映射）
```

## 依赖变更汇总

```
              当前              →    最终
网络库:      muduo             →    Boost.Asio (C++20 coroutine)
MySQL:       mysqlclient (C)   →    boost::mysql (异步)
Redis:       hiredis (同步)    →    hiredis+asio 或 Boost.Redis
日志:        muduo::Logging    →    spdlog 或 std::cout
JSON:        nlohmann/json     →    不变
图片处理:    OpenCV            →    不变
加密:        OpenSSL           →    不变

编译要求:
  C++20 (协程)
  Boost >= 1.82（Phase 1-3）/ >= 1.87（Phase 4 方案 B）
  GCC >= 10 或 Clang >= 13
  CMake >= 3.20
```

## 风险与注意事项

1. **Boost 版本要求**：协程支持需要 Boost 1.82+，boost::mysql 需要 Boost 1.82+
2. **编译时间**：Boost 头文件较多，编译时间会增加，可用 ccache 缓解
3. **二进制体积**：Boost 模板实例化会导致二进制增大
4. **调试**：协程调用栈不如同步代码直观，建议配合 GDB 的协程支持插件
5. **渐进迁移**：每个 Phase 必须确保编译运行通过后再进入下一阶段
6. **性能测试**：每个 Phase 完成后做简单的压力测试对比（如 `wrk` 或自定义客户端并发连接）
