# boost_asio协程服务器要点记录

## any_io_executor

类型擦除的执行器包装器，决定异步操作在哪里执行。

```cpp
executor_ = std::move(executor);  // 转移所有权，避免拷贝
```

**用途：**
- 创建 `tcp::socket` / `mysql::tcp_connection` 时传入，注册异步操作到 `io_context`
- 被 socket、timer、连接池等多处共享（内部是 shared_ptr 风格，复制只增加引用计数）

## co_spawn

协程启动器，将协程"发射"到执行器上运行。

```cpp
asio::co_spawn(ioc, my_coro(), asio::detached);
```

| 参数 | 含义 |
|------|------|
| `ioc` | 执行器，决定协程运行位置 |
| `my_coro()` | 返回 `awaitable<T>` 的协程函数 |
| `detached` | 完成策略，忽略返回值 |

**为什么需要：** 协程函数直接调用只返回 `awaitable` 对象，不执行。`co_spawn` 创建协程帧并调度第一次 resume。

## co_return vs return

```cpp
co_return value;  // 协程：设置返回值并结束协程帧
return value;     // 普通函数：直接返回
```

含 `co_await` / `co_return` 的函数被编译器变换为协程（堆上协程帧 + 状态机），必须用 `co_return`。

## mysql::tcp_connection 构造

```cpp
auto conn = std::make_shared<mysql::tcp_connection>(executor_);
```

执行器传给内部 `tcp::socket`，socket 用它注册异步操作到 epoll。

**每个连接：**
- 独立的 TCP socket 和 MySQL 会话
- 复制执行器（共享同一个 `io_context` 调度）

## bootstrap 为什么是协程

```cpp
asio::awaitable<void> bootstrap(...) {
    co_await init_db();      // 必须等待
    co_await init_redis();   // 必须等待
    server.start();
}
```

普通函数无法 `co_await`，异步初始化会变成"返回 awaitable 但不执行"。

## 回调 vs 协程的边界

```cpp
Session(socket, [](auto s, json js) { /* 业务逻辑 */ }, ...);
```

**回调的本质：依赖注入。** 允许外部注入行为，对象保持通用、可复用。

**协程的本质：控制流线性化。** 让异步操作看起来像同步代码。

| 场景 | 用协程 | 用回调 |
|------|--------|--------|
| 同一对象内的异步流程 | ✅ `co_await` 线性化 | ❌ 回调嵌套 |
| 跨对象的行为注入 | ❌ 无法传递"未来行为" | ✅ 函数指针/闭包 |

**两者不冲突：** 协程风格的回调（返回 `awaitable` 的回调函数）既有解耦，又有协程的线性化好处。

```cpp
// MessageCallback 签名是协程，但仍然是回调模式
using MessageCallback = std::function<asio::awaitable<void>(const Ptr&, json)>;
```

## Session ≈ muduo::TcpConnection

不是 Channel。Session 是完整的连接管理器：持有 socket、缓冲区、处理读写、管理发送队列。

asio 没有显式的 Channel 类，事件分发隐式在 `completion_handler` 闭包中。

## use_awaitable

完成标记，告诉 asio 用协程方式等待结果。

```cpp
co_await socket.async_read_some(buffer, use_awaitable);  // 返回 awaitable<size_t>
```

配合大括号匹配确定 TCP 流中的 JSON 边界（应用层协议实现）。

## AsyncConnectionGuard：工厂方法 + RAII

```cpp
auto guard = co_await AsyncConnectionGuard::create();
// 使用 guard->connection()
// 离开作用域，析构自动归还连接
```

**工厂方法解决两个问题：**
1. 构造函数私有，外部无法直接创建
2. 获取连接需要 `co_await`，普通构造函数无法做到

**RAII 保证：** 构造获取资源，析构释放资源，异常安全。

## 函数签名决定 co_return 行为

```cpp
asio::awaitable<T> foo() { co_return value; }
```

`co_return` 不自动包装返回值。`awaitable<T>::promise_type` 定义了 `return_value(T)`，`co_return` 调用它。

## this_coro::executor

```cpp
auto executor = co_await asio::this_coro::executor;
```

特殊 awaiter，不挂起协程，只读取当前协程的执行器。设计成 `co_await` 保证只能在协程内使用。

## ctx_ 含义

`ctx` = context（上下文）。hiredis 中表示连接对象：

| 类型 | 用途 |
|------|------|
| `redisContext` | 同步连接，阻塞调用 |
| `redisAsyncContext` | 异步连接，非阻塞 + 回调 |

## Redis 双连接设计

Redis 协议限制：SUBSCRIBE 模式的连接只能执行 SUBSCRIBE/UNSUBSCRIBE。

| 连接 | 模式 | 原因 |
|------|------|------|
| `publish_ctx_` | 同步 | 一次性发送，阻塞短，简单直接 |
| `sub_ctx_` | 异步 + 钩子 | 长期监听，融入事件循环 |

## hiredis 钩子：连接 hiredis 与 asio

hiredis 不知道 asio，通过钩子通知你"需要监控 fd"：

```cpp
sub_ctx_->ev.addRead = &Redis::ev_add_read;   // hiredis 调用 → 你注册 epoll 读事件
sub_ctx_->ev.delRead = &Redis::ev_del_read;   // hiredis 调用 → 你取消 epoll 读事件
```

**流程：**

```
hiredis: "需要监控可读" → ev.addRead()
    → asio::async_wait(read)
    → epoll 触发
    → redisAsyncHandleRead()
    → hiredis 处理数据
```

**stream_descriptor：** 包装 hiredis 的 fd，让 asio 能用 `async_wait` 监控。

---

## 技术设计优势

### 1. 状态内聚

```
回调模型：连接状态分散在 ChatServer 的 map，每次回调要查找
协程模型：每个 Session 自带 recvBuf_，状态与连接绑定
```

**优势：** 扩展新状态只需加 Session 成员，无需改全局结构。

### 2. 分层解耦

```
网络层（Session）   ← 不知道 ChatService 存在
业务层（ChatService）← 不知道 Session 实现
数据层（Model）      ← 不知道调用者
```

**优势：** 可独立测试、可复用、可替换实现。

### 3. RAII 资源安全

```cpp
{
  auto guard = co_await AsyncConnectionGuard::create();
  // 异常？正常返回？析构函数都归还连接
}
```

### 4. 单线程无锁

```cpp
unordered_map<int, Session::Ptr> _userConnMap;  // 单线程，无需 mutex
```

**优势：** 无锁竞争，无上下文切换，CPU 缓存友好。

### 5. 协程挂起 vs 线程阻塞

```
1000 个请求等待数据库连接：
  线程阻塞：1000 线程 × 8MB 栈 = 8GB
  协程挂起：1000 协程帧 × ~1KB = 1MB
```

### 6. 分布式支持

```
Server A ←──→ Redis pub/sub ←──→ Server B
         跨服务器消息同步已实现
```

**优势：** 水平扩展，高可用，无单点故障。

---

## 架构对比：多线程 vs 多进程

| 方案 | 适用场景 | 代表 |
|------|----------|------|
| 单进程单线程 | <1万用户，学习项目 | 当前项目 |
| 单进程多线程 | 1-10万用户，单机够用 | 游戏服务器 |
| 多进程分布式 | 10万+用户，需扩展 | 微信、Discord |

**当前架构：** 单线程 + 多进程部署 + Redis 消息同步，适合学习和中小规模生产。

---

## 总结

**一句话：** 基于 C++20 协程和 Boost.Asio 的高并发聊天服务器，从 muduo 回调迁移到全协程架构，实现网络层、数据库、Redis 完全异步化。

**核心亮点：**

1. **协程化数据库：** boost::mysql + 连接池，co_await 挂起协程而非阻塞线程
2. **hiredis 集成 asio：** 事件钩子将 fd 注册到 epoll，实现订阅协程化
3. **回调与协程边界：** MessageCallback 是协程签名但仍是回调模式，实现解耦

**关键数字：** 16+ 业务 handler、单线程 io_context、C++20 + Boost 1.82+
