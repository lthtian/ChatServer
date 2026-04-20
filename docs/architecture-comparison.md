# Phase 1 技术架构对比：muduo → Boost.Asio 协程

---

## 1. 整体架构对比

### 1.1 修改前（muduo 回调模型）

```
┌──────────────────────────────────────────────────────────────────┐
│  main()                                                          │
│    EventLoop loop;                                               │
│    ChatServer(&loop, addr, "chat");                              │
│    loop.loop();  ←── 阻塞在 epoll_wait，事件驱动                 │
└──────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│  muduo::net::TcpServer  (内部管理)                               │
│    setThreadNum(2) → 创建 2 个 IO 线程，每个线程一个 EventLoop   │
│                                                                  │
│    主 EventLoop ─── epoll_wait ───┐                              │
│    IO 线程 1   ─── epoll_wait ───┤ 监听同一个 listen fd          │
│    IO 线程 2   ─── epoll_wait ───┘ (通过 eventfd 唤醒)           │
│                                                                  │
│    回调注册:                                                      │
│      setConnectionCallback → onConnection                        │
│      setMessageCallback    → onMessage                           │
└──────────────────────────────────────────────────────────────────┘
            │ 新连接到达                │ 数据到达
            ▼                          ▼
┌──────────────────────────────────────────────────────────────────┐
│  ChatServer (回调驱动)                                           │
│                                                                  │
│    onConnection(conn):                                           │
│      if (!conn->connected())                                     │
│        → ChatService::clientCloseException(conn)                 │
│        → conn->shutdown()                                        │
│                                                                  │
│    onMessage(conn, buffer, timestamp):                           │
│      buf = buffer->retrieveAllAsString()                         │
│      _recvBuffers[conn] += buf    ←── 连接级缓冲区在 ChatServer  │
│      大括号匹配提取完整 JSON                                      │
│      → 解析 JSON，查 handler                                     │
│      → 创建 Task{conn, js, time, handler}                        │
│      → _taskQueue.push(task)     ←── 投递到业务线程池             │
│                                                                  │
│    _recvBuffers: unordered_map<TcpConnectionPtr, string>         │
│      所有连接的缓冲区集中管理在 ChatServer 中                     │
└──────────────────────────────────────────────────────────────────┘
                              │
                              ▼ push(Task)
┌──────────────────────────────────────────────────────────────────┐
│  TaskQueue<Task>  (生产者-消费者)                                 │
│    mutex + condition_variable 实现的阻塞队列                      │
│                                                                  │
│    生产者: IO 线程（onMessage 中 push）                           │
│    消费者: WorkerThreadPool 的 N 个工作线程                       │
└──────────────────────────────────────────────────────────────────┘
                              │ pop(Task)
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│  WorkerThreadPool (N = hardware_concurrency - 2, 最少 4)         │
│                                                                  │
│    workerFunc() {                                                │
│      while (running) {                                           │
│        task = _taskQueue.pop();      ←── 阻塞等待                │
│        if (!task.conn->connected())  ←── 检查连接有效性           │
│          continue;                                               │
│        task.handler(task.conn, task.js, task.time);              │
│        // handler 内部调用 conn->send(response.dump())           │
│      }                                                           │
│    }                                                             │
│                                                                  │
│    线程 1 ── pop() ── handler() ── send()                        │
│    线程 2 ── pop() ── handler() ── send()                        │
│    ...                                                           │
│    线程 N ── pop() ── handler() ── send()                        │
└──────────────────────────────────────────────────────────────────┘
```

**线程模型：**
```
进程
├── Main 线程       → EventLoop::loop() (epoll_wait)
├── IO 线程 1       → EventLoop::loop() (epoll_wait)
├── IO 线程 2       → EventLoop::loop() (epoll_wait)
├── Worker 线程 1   → taskQueue.pop() 阻塞 → handler() 同步执行
├── Worker 线程 2   → taskQueue.pop() 阻塞 → handler() 同步执行
├── ...
└── Worker 线程 N   → taskQueue.pop() 阻塞 → handler() 同步执行

Redis 订阅线程     → redisGetReply() 阻塞
总计: 3 + N + 1 个线程
```

---

### 1.2 修改后（Boost.Asio 协程模型）

```
┌──────────────────────────────────────────────────────────────────┐
│  main()                                                          │
│    asio::io_context ioc;                                         │
│    ChatServer(ioc, ip, port);                                    │
│    ioc.run();  ←── 阻塞在 epoll_wait，事件驱动                   │
└──────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│  ChatServer                                                      │
│    tcp::acceptor acceptor_;                                      │
│                                                                  │
│    start():                                                      │
│      co_spawn(ioc_, do_accept(), detached)                       │
│                                                                  │
│    do_accept() 协程:                                             │
│      while (true) {                                              │
│        socket = co_await acceptor_.async_accept(use_awaitable)   │
│        // ─── 协程在这里挂起，不阻塞线程 ───                      │
│        // 新连接到来时，协程在此处恢复执行                         │
│        session = make_shared<Session>(                           │
│            std::move(socket),                                    │
│            messageCallback,  // 收到消息 → 创建Task → push队列   │
│            closeCallback     // 连接断开 → clientCloseException  │
│        )                                                         │
│        session->start()  // 启动该连接的 read_loop 协程          │
│      }                                                           │
│      // 每个新连接创建一个独立的 Session + 独立的 read_loop 协程  │
└──────────────────────────────────────────────────────────────────┘
              │                          │
     ┌────────┴────────┐        ┌───────┴───────┐
     ▼                 ▼        ▼               ▼
┌──────────┐    ┌──────────┐  ┌──────────┐  ┌──────────┐
│ Session 1│    │ Session 2│  │ Session 3│  │ Session N│
│          │    │          │  │          │  │          │
│read_loop │    │read_loop │  │read_loop │  │read_loop │
│ 协程运行 │    │ 协程运行 │  │ 协程运行 │  │ 协程运行 │
│  中...   │    │  中...   │  │  中...   │  │ 挂起中   │
│          │    │          │  │          │  │          │
│recvBuf_  │    │recvBuf_  │  │recvBuf_  │  │recvBuf_  │
│writeQueue│    │writeQueue│  │writeQueue│  │writeQueue│
└──────────┘    └──────────┘  └──────────┘  └──────────┘
     │                 │           │               │
     └────────┬────────┘           └───────┬───────┘
              ▼                            ▼
     回调触发 → 创建 Task          回调触发 → 创建 Task
              │                            │
              ▼                            ▼
┌──────────────────────────────────────────────────────────────────┐
│  TaskQueue<Task> + WorkerThreadPool    ←── 与 muduo 版完全相同    │
│                                                                  │
│    Task { Session::Ptr, json, Timestamp, MsgHandler }            │
│                                                                  │
│    线程 1 ── pop() ── handler() ── session->send()               │
│    线程 2 ── pop() ── handler() ── session->send()               │
│    ...                                                           │
└──────────────────────────────────────────────────────────────────┘
```

**线程模型：**
```
进程
├── IO 线程 (1个)   → io_context::run() (epoll_wait)
│                     同时运行所有协程:
│                     - do_accept() 协程
│                     - Session 1 的 read_loop() 协程
│                     - Session 2 的 read_loop() 协程
│                     - Session N 的 read_loop() 协程
│                     - 所有 async_write 的完成回调
│
├── Worker 线程 1   → taskQueue.pop() 阻塞 → handler() 同步执行
├── Worker 线程 2   → taskQueue.pop() 阻塞 → handler() 同步执行
├── ...
└── Worker 线程 N   → taskQueue.pop() 阻塞 → handler() 同步执行

Redis 订阅线程     → redisGetReply() 阻塞
总计: 1 + N + 1 个线程
```

---

## 2. 新连接接入流程对比

### 2.1 muduo 版本

```
时间线 →

客户端                内核 TCP 栈          muduo IO线程           ChatServer
  │                     │                    │                     │
  │─── SYN ───────────→│                     │                     │
  │←── SYN+ACK ────────│                     │                     │
  │─── ACK ───────────→│                     │                     │
  │                     │── 可读事件 ────────→│                     │
  │                     │                    │ muduo 内部 accept() │
  │                     │                    │ 创建 TcpConnection  │
  │                     │                    │ 调用 onConnection() │
  │                     │                    │────────────────────→│
  │                     │                    │                     │ conn->connected()==true
  │                     │                    │                     │ (什么都不做)
  │                     │                    │                     │
  │─── 数据 ──────────→│                     │                     │
  │                     │── 可读事件 ────────→│                     │
  │                     │                    │ muduo 从 socket 读取 │
  │                     │                    │ 调用 onMessage()    │
  │                     │                    │────────────────────→│
  │                     │                    │                     │ 从 buffer 取数据
  │                     │                    │                     │ JSON 解析
  │                     │                    │                     │ 创建 Task
  │                     │                    │                     │ push 到队列
```

**关键点：** 新连接的创建、回调绑定、数据读取全部由 muduo 框架内部控制，开发者只注册两个回调函数。IO 线程可能同时处理多个连接的事件（epoll 多路复用）。

### 2.2 Asio 协程版本

```
时间线 →

客户端              内核 TCP 栈         io_context 线程        Session 协程
  │                   │                    │                     │
  │─── SYN ──────────→│                    │                     │
  │←── SYN+ACK ───────│                    │                     │
  │─── ACK ──────────→│                    │                     │
  │                   │── 可读事件 ────────→│                     │
  │                   │                    │ do_accept() 协程恢复 │
  │                   │                    │ acceptor_.async_accept()
  │                   │                    │ 返回新 socket        │
  │                   │                    │ 创建 Session 对象    │
  │                   │                    │ session->start()     │
  │                   │                    │────────────────────→│
  │                   │                    │                     │ 启动 read_loop() 协程
  │                   │                    │                     │ co_await async_read_some()
  │                   │                    │                     │ (协程挂起，归还线程)
  │                   │                    │                     │
  │─── 数据 ──────────→│                    │                     │
  │                   │── 可读事件 ────────→│                     │
  │                   │                    │                     │ read_loop() 协程恢复
  │                   │                    │                     │ 读取数据到 recvBuf_
  │                   │                    │                     │ JSON 解析
  │                   │                    │                     │ 调用 messageCallback
  │                   │                    │                     │ → 创建 Task
  │                   │                    │                     │ → push 到队列
  │                   │                    │                     │ 继续下一次 co_await
  │                   │                    │                     │ (协程再次挂起)
```

**关键点：** 每个连接有自己独立的 `read_loop()` 协程，协程在 `co_await async_read_some()` 处挂起，等待数据到来时恢复。挂起时不消耗线程资源。

---

## 3. 请求处理流程对比

### 3.1 muduo: 回调链

```
一次完整的 "客户端发送登录请求 → 服务器响应" 流程:

    IO 线程                                    Worker 线程
    ────────                                  ──────────
    epoll 唤醒: socket 可读
        │
    onMessage(conn, buffer, time)
        │
    ├── buffer->retrieveAllAsString()
    ├── _recvBuffers[conn] += data
    ├── 大括号匹配 → 提取完整 JSON
    ├── json::parse(json_str)
    ├── getHandler(msgid)
    ├── Task{conn, js, time, handler}
    ├── _taskQueue.push(task)  ──────────→  taskQueue.pop()
        │                                      │
        │ (IO线程立即返回，继续处理其他连接)    ├── 检查 conn->connected()
        │                                      ├── handler(session, js, time)
        │                                      │   └── login():
        │                                      │       查询数据库 (阻塞)
        │                                      │       更新状态
        │                                      │       conn->send(response)
        │                                      │       (muduo 保证线程安全)
        │                                      │
        │                                      └── 继续取下一个 task
        │
        ↓
    (与此同时，IO 线程可能在处理
     其他连接的 onMessage/onConnection)
```

**特点：**
- IO 线程负责读取 + 解析 + 分发，然后立即返回处理其他连接
- 业务逻辑在工作线程中**同步阻塞**执行（数据库查询会阻塞线程）
- `conn->send()` 由 muduo 保证线程安全（内部通过 EventLoop 序列化写操作）

### 3.2 Asio: 协程 + 回调混合

```
一次完整的 "客户端发送登录请求 → 服务器响应" 流程:

    io_context 线程                           Worker 线程
    ──────────────                            ──────────

    Session::read_loop() 协程被唤醒
        │
    co_await async_read_some() 返回
        │
    ├── recvBuf_.append(data)
    ├── 大括号匹配 → 提取完整 JSON
    ├── json::parse(json_str)
    ├── messageCallback(session, js)
    │   └── 在 ChatServer 的 lambda 中:
    │       ├── getHandler(msgid)
    │       ├── Task{session, js, time, handler}
    │       └── _taskQueue.push(task)  ───→  taskQueue.pop()
        │                                      │
    read_loop() 继续循环                        ├── 检查 session->connected()
    co_await async_read_some()                  ├── handler(session, js, time)
    (协程挂起，等待下一个数据)                   │   └── login():
        │                                      │       查询数据库 (阻塞)
        │                                      │       更新状态
        │                                      │       session->send(response)
    (io_context 线程空闲，                     │       └── post 到 io_context
     可以运行其他协程)                          │           └── async_write
        │                                      │
        ↓                                      └── 继续取下一个 task
```

**特点：**
- `read_loop()` 是协程，在 `co_await` 处挂起后 **io_context 线程完全空闲**，可以运行其他 Session 的协程
- 业务逻辑仍然在工作线程中同步阻塞执行（Phase 1 未改变）
- `session->send()` 通过 `asio::post` + 写队列保证线程安全

---

## 4. 协程到底做了什么

### 4.1 协程的本质

C++20 协程是一个可以被**挂起（suspend）和恢复（resume）**的函数。当一个协程执行到 `co_await` 时：

```
                  协程执行流
                  ══════════

    read_loop() {
        ...
        n = co_await async_read_some(buf);
            ──────────────────────────────
            │                             │
            │  1. 发起异步读操作           │
            │  2. 保存当前执行状态          │
            │     (局部变量、挂起点)        │
            │  3. 返回给调用者              │
            │     ↓                        │
            │  io_context 线程空闲          │
            │  可以做其他事情               │
            │  - 运行其他 Session 的协程    │
            │  - 处理 async_write 完成回调  │
            │  - accept 新连接              │
            │                             │
            │  ... 时间流逝 ...             │
            │  数据到达，epoll 唤醒         │
            │                             │
            │  4. 恢复协程执行状态          │
            │  5. n = 实际读取的字节数      │
            │  6. 继续往下执行              │
            ──────────────────────────────

        // 继续处理数据
        recvBuf_.append(buf, n);
        // JSON 解析...
        // 继续下一次循环
    }
    ```

**对比回调版本做同样的事：**

```cpp
// muduo 回调版本: IO线程 → onMessage回调 → 处理数据 → 返回
// 每次数据到来，muduo 调用一次 onMessage，处理完返回

void ChatServer::onMessage(conn, buffer, time) {
    string buf = buffer->retrieveAllAsString();
    _recvBuffers[conn] += buf;
    // ... JSON 解析 ...
    // 处理完毕，函数返回，IO 线程去做别的事
}
```

两者在 I/O 层面做的事情**完全相同**：都是异步通知 + 非阻塞 I/O。区别在于代码的组织方式。

### 4.2 协程的具体作用

#### 作用 1: 状态内聚 — 每个连接的上下文集中管理

```
修改前: 连接状态分散在多处
┌─────────────────────────────────────────────┐
│ ChatServer                                  │
│   _recvBuffers[TcpConn1] = "半截JSON..."    │  ← 缓冲区在 ChatServer 的 map 里
│   _recvBuffers[TcpConn2] = ""               │
│   _recvBuffers[TcpConn3] = "{name:"         │
│                                             │
│   onMessage(conn, buf, time) {              │  ← 回调函数是无状态的
│     // 每次调用都要从 map 查找缓冲区         │
│     // 通过 conn 指针关联到正确的 buffer     │
│   }                                         │
└─────────────────────────────────────────────┘

修改后: 每个连接自带完整上下文
┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│ Session 1        │  │ Session 2        │  │ Session 3        │
│ socket_          │  │ socket_          │  │ socket_          │
│ recvBuf_ = "..." │  │ recvBuf_ = ""    │  │ recvBuf_ = "{"   │
│ writeQueue_      │  │ writeQueue_      │  │ writeQueue_      │
│ closed_ = false  │  │ closed_ = false  │  │ closed_ = false  │
│                  │  │                  │  │                  │
│ read_loop() 协程 │  │ read_loop() 协程 │  │ read_loop() 协程 │
│ 自带执行上下文   │  │ 自带执行上下文   │  │ 自带执行上下文   │
└──────────────────┘  └──────────────────┘  └──────────────────┘
```

**好处：** 协程的局部变量天然保留在协程帧（coroutine frame）中。`recvBuf_` 是 Session 的成员，与连接绑定，不需要用 map 查找。如果未来需要为每个连接维护更多状态（如认证信息、心跳计时器），直接加在 Session 类里即可。

#### 作用 2: 线性代码流 — 消除回调的"控制流反转"

```cpp
// ═══ 修改前: 回调风格 (控制流由框架驱动) ═══
//
// onMessage 是一个"被调用"的函数，开发者不控制何时被调用
// 如果要实现"读取 → 解析 → 如果不完整则继续读 → 完整后处理"
// 在回调中需要手动管理状态:
//
void onMessage(conn, buffer, time) {
    _recvBuffers[conn] += buffer->retrieveAllAsString();
    // 手动管理大括号匹配状态
    // 手动处理"不完整则等下次回调"的逻辑
    // 手动从 map 查找对应的缓冲区
}

// ═══ 修改后: 协程风格 (看起来像同步代码) ═══
//
// read_loop() 是一个"主动"的循环，开发者完全控制流程
//
asio::awaitable<void> Session::read_loop() {
    char buf[4096];
    while (true) {                                // 线性循环
        size_t n = co_await async_read_some(...);  // "等待"数据，实际挂起
        recvBuf_.append(buf, n);                  // 直接操作自己的缓冲区
        // JSON 解析...
        // 处理...
    }                                             // 循环继续
}
```

虽然 Phase 1 的 JSON 解析逻辑看起来差不多（因为是直接搬过来的），但协程的优势会在 **Phase 2（handler 协程化）** 中真正体现：

```cpp
// Phase 2 之后，handler 也可以写成线性代码:
asio::awaitable<void> ChatService::login(Session::Ptr session, json& js) {
    // 直接 co_await 数据库查询，不需要回调
    auto user = co_await _userModel.query(name);

    if (user.valid()) {
        co_await _userModel.updateState(user);  // 直接 co_await 更新
        session->send(response.dump());
    }
}
// 整个流程是线性的，没有回调嵌套
```

#### 作用 3: 高效的挂起 — 不消耗线程

```
假设有 1000 个空闲连接（已登录但没有人发消息）:

修改前 (muduo):
  - 这些连接在 epoll 中注册，不消耗 CPU
  - 但每个连接的状态（缓冲区）存在 ChatServer 的 map 中
  - map 查找开销: O(1) 但需要哈希计算 + mutex
  - 实际内存: TcpConnection 对象 + map 条目

修改后 (Asio 协程):
  - 每个连接的 read_loop() 协程挂起在 co_await 处
  - 协程帧在堆上分配（约几百字节），不占用栈空间
  - 不消耗线程（1000 个挂起的协程 ≠ 1000 个线程）
  - 实际内存: Session 对象 + 协程帧

  ┌─────────────────────────────────────────────────┐
  │  io_context 线程 (1个)                           │
  │                                                  │
  │  epoll_wait → 有事件 → 恢复对应协程 → 处理 → 挂起 │
  │                                                  │
  │  挂起的协程不消耗线程，只占堆内存                   │
  │                                                  │
  │  活跃协程1 → 活跃协程2 → ... → epoll_wait        │
  │                                                  │
  │  协程1001~1000: 全部挂起，不参与调度               │
  └─────────────────────────────────────────────────┘
```

---

## 5. 线程安全的 send() 对比

### 5.1 muduo 的 TcpConnection::send()

```
Worker 线程调用 conn->send(msg):
  │
  ├── muduo 内部:
  │     如果当前在 IO 线程 → 直接写入
  │     如果在其他线程 → runInLoop(实际写操作)
  │       → 通过 eventfd 通知 IO 线程
  │       → IO 线程的 EventLoop 在下一轮 poll 时执行写操作
  │
  └── muduo 保证: 所有写操作最终在 IO 线程串行执行
```

### 5.2 Session::send()

```
Worker 线程调用 session->send(msg):
  │
  ├── asio::post(socket_.get_executor(), lambda):
  │     将 lambda 投递到 io_context 的任务队列
  │     io_context 线程在下一轮事件循环时执行 lambda
  │
  ├── lambda 内部:
  │     if (closed_) return;          // 检查连接是否已关闭
  │     writeQueue_.push(msg);        // 加入写队列
  │     if (writeQueue_.size() == 1)  // 之前队列为空 = 没有写操作在进行
  │       do_write();                 // 启动异步写链
  │
  └── do_write() 保证: 同一时间只有一个 async_write 在执行

写队列保证顺序:
  push("消息A") → push("消息B") → push("消息C")
  ─────────────────────────────────────────────
  async_write("消息A") ──完成──→ async_write("消息B") ──完成──→ async_write("消息C")
  (严格串行，不会交错)
```

---

## 6. 连接断开处理对比

### 6.1 muduo

```
客户端断开连接:
  │
  ├── TCP FIN 到达
  ├── epoll 通知 IO 线程
  ├── muduo 检测到连接断开
  ├── 调用 onConnection(conn)
  │     conn->connected() == false
  │     → ChatService::clientCloseException(conn)
  │       → 遍历 _userConnMap 找到该 conn 对应的 userId
  │       → 更新数据库状态为 offline
  │       → Redis unsubscribe
  │     → conn->shutdown()
```

### 6.2 Asio 协程

```
客户端断开连接:
  │
  ├── TCP FIN 到达
  ├── epoll 通知 io_context
  ├── read_loop() 协程中 co_await async_read_some() 抛异常
  │     (boost::system::system_error: connection reset / eof)
  ├── 协程 catch 块捕获异常
  ├── 协程退出 while(true) 循环
  ├── 协程末尾调用 close()
  │     closed_ 原子操作设为 true (防重复调用)
  │     → closeCallback_(shared_from_this())
  │       → ChatService::clientCloseException(session)
  │         → 遍历 _userConnMap 找到该 session 对应的 userId
  │         → 更新数据库状态为 offline
  │         → Redis unsubscribe
  │     → socket_.close()
  │
  ├── 协程结束，协程帧被销毁
  └── Session 的 shared_ptr 引用计数归零，Session 对象被销毁
       → 析构函数调用 close()（但 closed_ 已经是 true，直接返回）
```

---

## 7. 信号处理与优雅关闭对比

```
修改前:                                    修改后:
───────────────────────────────────────────────────────────
SIGINT 到达                                SIGINT 到达
  → resetHandler()                           → resetHandler()
    → ChatService::reset()                     → ChatService::reset()
      → 所有在线用户状态设为 offline              → 所有在线用户状态设为 offline
    → g_loop->quit()                         → g_ioc->stop()
      → EventLoop 退出 loop()                  → io_context 停止 run()
      → main() 返回                             → main() 返回
                                                 → ChatServer 析构
                                                   → WorkerThreadPool::shutdown()
                                                   → acceptor_.close()
                                                   → 所有 Session 最终因
                                                     socket 关闭而退出协程
```

---

## 8. 总结: 协程在本项目中的实际价值

| 维度 | Phase 1 的实际效果 | 未来阶段的价值 |
|------|-------------------|---------------|
| **代码组织** | Session 类封装了连接的所有状态，比散落在 ChatServer 的 map 更清晰 | Phase 2: handler 协程化后，业务代码变为线性流 |
| **线程效率** | 当前与 muduo 相当（1个 IO 线程 vs 2-3个） | Phase 3: DB 异步化后，不再需要 WorkerThreadPool |
| **内存效率** | 挂起的协程只占协程帧（~几百字节），不占线程栈 | 高并发时优势明显：1000 空闲连接不需要额外线程 |
| **性能** | Phase 1 与 muduo 基本持平 | Phase 2-4 逐步消除同步阻塞，性能上限更高 |
| **可扩展性** | 协程模型为后续异步化铺平了道路 | Phase 2-4 可以逐步替换，每步独立验证 |

**Phase 1 的核心成果：** 在保持业务逻辑零改动的前提下，完成了网络层的底层替换。所有 muduo 依赖已移除，项目可以基于 Boost.Asio 继续演进。
