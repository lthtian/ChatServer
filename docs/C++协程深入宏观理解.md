# C++20 协程深入宏观理解

---

## 核心观念

**协程在表面上是在函数中的暂停节点，本质是在堆上存储函数上下文数据的机制。**

**协程的终极目标是增加单个线程的资源利用率**——让线程在等待 I/O 的时间片内去做别的事情，而不是阻塞空转。当所有阻塞操作（网络、数据库、Redis）都可以 `co_await` 时，一个线程就能同时推进数千个并发任务，而不需要数千个线程。

C++20 协程是**无栈协程（stackless）**，没有独立的栈。编译器将协程函数变换为一个**堆上的结构体（协程帧）+ 一个状态机函数**。协程帧保存所有跨越 `co_await` 的局部变量，状态机通过 `switch/goto` 实现恢复跳转。

---

## 与 muduo 回调的本质对比

### muduo：回调 + 全局状态

```
epoll 触发 socket_fd 可读
  → 调用 onMessage(conn, buffer, time)
    → 回调函数拿到的上下文 = 函数参数 + 全局 map 查找
    → 需要手动从 _recvBuffers[conn] 找到该连接的缓冲区
    → 回调本身是无状态的，不记得"上次处理到哪了"
```

一个线程通过 epoll 管理多个连接，回调函数只能通过参数和全局变量找到目标数据。

### asio 协程：协程帧 + 状态机

```
epoll 触发 socket_fd 可读
  → asio 调用 completion_handler 闭包
    → 闭包内部调用 coroutine_handle.resume()
      → 跳回协程挂起点
      → recvBuf_ 等变量就在协程帧里，直接用
      → 不需要 map，不需要手动查找
```

同样的 epoll 多路复用，但通过协程帧自动保存了每个连接的上下文，代码设计更简单直观。

---

## 协程的三大作用

### 作用 1: 状态内聚 — 每个连接的上下文集中管理

```
muduo: 连接状态分散在多处
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

asio 协程: 每个连接自带完整上下文
┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│ Session 1        │  │ Session 2        │  │ Session 3        │
│ socket_          │  │ socket_          │  │ socket_          │
│ recvBuf_ = "..." │  │ recvBuf_ = ""    │  │ recvBuf_ = "{"   │
│ writeQueue_      │  │ writeQueue_      │  │ writeQueue_      │
│ closed_ = false  │  │ closed_ = false  │  │ closed_ = false  │
│ read_loop() 协程 │  │ read_loop() 协程 │  │ read_loop() 协程 │
│ 自带执行上下文   │  │ 自带执行上下文   │  │ 自带执行上下文   │
└──────────────────┘  └──────────────────┘  └──────────────────┘
```

`recvBuf_` 是 Session 的成员，与连接绑定，不需要用 map 查找。如果未来需要为每个连接维护更多状态（认证信息、心跳计时器等），直接加在 Session 类里即可。

### 作用 2: 线性代码流 — 消除回调的"控制流反转"

```cpp
// muduo 回调: 控制流由框架驱动，开发者被动响应
void onMessage(conn, buffer, time) {
    _recvBuffers[conn] += buffer->retrieveAllAsString();
    // 手动管理大括号匹配状态
    // 手动从 map 查找对应的缓冲区
}

// asio 协程: 开发者主动控制流程，看起来像同步代码
asio::awaitable<void> Session::read_loop() {
    char buf[4096];
    while (true) {                                // 线性循环
        size_t n = co_await async_read_some(...);  // "等待"数据，实际挂起
        recvBuf_.append(buf, n);                  // 直接操作自己的缓冲区
        // JSON 解析... 处理... 循环继续
    }
}

// Phase 2 之后，handler 也可以线性化:
asio::awaitable<void> ChatService::login(Session::Ptr session, json& js) {
    auto user = co_await _userModel.query(name);     // 直接 co_await 数据库
    if (user.valid()) {
        co_await _userModel.updateState(user);
        session->send(response.dump());
    }
}
```

### 作用 3: 最大化单线程资源利用率 — 核心价值

注意：muduo 通过 epoll 已经做到了网络 I/O 不阻塞线程，所以仅在网络层面，协程并不比 epoll+回调有资源效率优势。协程的真正价值在于：**当所有阻塞操作（网络、数据库、Redis）都可以 co_await 时，一个线程就能同时推进数千个并发任务**。

```
线程阻塞模型（协程要替代的目标）:
═══════════════════════════════════
每个连接一个线程，所有 I/O 阻塞线程

void handle_client(socket) {
    while (true) {
        data = socket.read();          // 阻塞
        result = db.query(sql);        // 阻塞
        socket.write(response);        // 阻塞
    }
}

10,000 个连接 = 10,000 个线程
内存: 10,000 × 8MB(栈) = 80GB
切换: 内核级上下文切换 ≈ μs

协程模型:
═══════════
所有连接共享少量线程，I/O 时挂起协程

asio::awaitable<void> handle_client(socket) {
    while (true) {
        data = co_await socket.async_read();     // 挂起，不阻塞线程
        result = co_await db.async_query(sql);   // 挂起，不阻塞线程
        co_await socket.async_write(response);   // 挂起，不阻塞线程
    }
}

10,000 个连接 = 10,000 个协程 + 1~几个线程
内存: 10,000 × ~1KB(协程帧) = 10MB
切换: 函数返回+调用 ≈ ns
```

| 维度 | 10,000 线程 | 10,000 协程 |
|------|------------|------------|
| 内存 | ~80GB（仅栈） | ~10MB（协程帧） |
| 切换开销 | 内核级 ~μs | 用户态 ~ns |
| 并发上限 | 几千个线程就到瓶颈 | 百万级协程可行 |

用本项目举例——Phase 1 vs Phase 3 的差异：

```
当前 Phase 1（网络层协程，数据库仍阻塞）:
  io_context 线程 1 个（网络 I/O 非阻塞）
  Worker 线程 4 个（数据库查询阻塞线程）

  1000 个用户同时登录 → 4 个 worker 线程都在阻塞等数据库
  → 第 5 个请求开始排队
  → 并发能力 = worker 线程数

Phase 3 完成后（数据库也协程化）:
  io_context 线程 1~2 个

  1000 个 login 协程全部 co_await db.query() 挂起
  没有一个线程被阻塞
  → 线程只负责处理 epoll 事件和协程调度
  → 并发能力 ≈ 协程数（几乎只受内存限制）
```

**并发能力演进：**

```
线程阻塞模型          epoll + 回调/协程        全异步协程
  │                    │                      │
  │ 并发 = 线程数       │ 网络 I/O 不阻塞       │ 所有 I/O 都不阻塞
  │ 几千就到瓶颈        │ 但 DB 操作仍阻塞      │ DB/Redis 全部 co_await
  │ 内存开销巨大        │ 需要 worker 线程池    │ 不需要 worker 线程池
  │                    │ 并发 = worker 线程数   │ 并发 = 协程数
  │                    │                      │
  ▼                    ▼                      ▼
 几千               几千~几万              几十万~百万
```

---

## 协程帧的本质

编译器将协程函数变换为：

**1. 堆上的结构体（协程帧）**：保存跨越 `co_await` 的局部变量和状态索引

```cpp
// 编译器内部生成的等价结构（简化）
struct __read_loop_frame {
    int __state = 0;          // 0=初始, 1=挂在co_await处, 2=已结束

    char read_buf[4096];      // 跨越挂起点的局部变量
    size_t n;
    size_t start_pos;
    int brace_count;
    bool in_string;

    Session* __this;          // this 指针
    // ... 协程基础设施 ...
};
```

**2. 状态机函数**：通过 `switch(__state)` + `goto` 实现恢复跳转

```cpp
void __read_loop_resume(__read_loop_frame* frame) {
    switch (frame->__state) {
        case 0: goto __start;
        case 1: goto __after_await;
    }

__start:
    while (true) {
        frame->__state = 1;
        socket.async_read_some(buf, [frame](error_code ec, size_t n) {
            frame->n = n;
            __read_loop_resume(frame);  // 恢复点
        });
        return;  // 挂起 = 函数返回，栈帧销毁，线程自由

__after_await:
        recvBuf_.append(frame->read_buf, frame->n);
        // ... 继续处理 ...
    }
}
```

**关键点：**
- 栈帧确实会销毁，协程帧（堆上结构体）才是数据的"记忆"
- 挂起 = 函数 `return`，线程回到 epoll 处理其他事件
- 恢复 = 用保存的 `__state` 值通过 `switch/goto` 跳回正确位置
- 不涉及线程切换，全在同一个线程上

---

## 与 Go goroutine 的区别

```
Go goroutine (有栈协程):
┌──────────────────────────┐
│ 独立的栈 (2KB~1GB 动态增长) │
│ ┌──────────────────────┐ │
│ │ read_buf[4096]       │ │  ← 在自己的栈上
│ │ 局部变量             │ │
│ │ 函数调用链           │ │
│ └──────────────────────┘ │
│ 保存/恢复所有寄存器       │
└──────────────────────────┘
挂起：保存寄存器，切换到另一个 goroutine 的栈

C++20 协程 (无栈协程):
┌──────────────────────────┐
│ 协程帧 (堆上结构体)       │
│ __state = 1              │  ← 整数标记"到了哪一步"
│ read_buf[4096]           │  ← 只有跨越挂起点的变量
│ n, start_pos, ...        │
│ (没有栈，没有函数调用链)  │
└──────────────────────────┘
挂起：函数 return，不需要保存寄存器
```

---

## co_await 展开机制

`size_t n = co_await socket_.async_read_some(buf, use_awaitable);` 被展开为三步：

**第一步：创建 awaitable 对象**
```cpp
auto awaitable = socket_.async_read_some(buf, use_awaitable);
// use_awaitable 告诉 asio "我要用协程方式等待结果"
```

**第二步：检查是否需要挂起**
```cpp
if (!awaitable.await_ready()) {
    // 内核缓冲区已有数据 → 直接返回，不挂起
    // 没有数据 → 进入挂起流程
```

**第三步：挂起 + 注册到 epoll**
```cpp
awaitable.await_suspend(coroutine_handle);
// coroutine_handle 指向当前协程帧，是恢复协程的"遥控器"
// await_suspend 内部创建闭包并注册到 epoll:
//
//   auto handler = [coroutine_handle](error_code ec, size_t n) {
//       coroutine_handle.resume();  // 恢复协程
//   };
//   epoll_ctl(ADD, socket_fd, handler);

return;  // 函数返回，栈帧销毁
```

---

## 完整链路图：从注册到恢复

```
                    注册阶段                           恢复阶段
                    ════════                           ════════

co_await async_read_some()                     epoll_wait() 返回
        │                                            │
        ▼                                            ▼
awaitable.await_ready()?                       内核通知 socket_fd 可读
  数据已就绪? → 直接返回                        │
  没有就绪 → 继续                              ▼
        │                                    asio reactor 查找:
        ▼                                    socket_fd → completion_handler
awaitable.await_suspend(handle)                     │
        │                                            ▼
        ├── 创建 completion_handler 闭包:      completion_handler(ec, n)
        │   [handle](ec, n) {                         │
        │     handle.resume();                        ▼
        │   }                                   handle.resume()
        │                                           │
        ├── 注册到 epoll:                            ▼
        │   socket_fd ↔ completion_handler     __read_loop_resume(frame)
        │                                           │
        └── return;  ←── 栈帧销毁                    ▼
                                             switch(frame->__state)
                                             → goto __after_await
                                             frame->n = ...
                                             recvBuf_.append(...)
                                             // 继续执行...
```

**socket 和协程暂停点之间的"联系"就是 completion_handler 闭包：** 它捕获了 `coroutine_handle`，注册到 epoll，当 socket 事件触发时被调用，从而恢复协程。

---

## muduo 与 asio 的等价关系

```
muduo                                        asio 协程
═════════════════════════════════════════════════════════════

epoll_ctl(ADD, fd, &channel)                 epoll_ctl(ADD, fd, ...) ← 一样的

channel 持有:                                reactor 持有:
  onMessage 函数指针                           completion_handler 闭包
  conn (TcpConnectionPtr)                     coroutine_handle (协程帧指针)

epoll 触发时:                                epoll 触发时:
  channel->handleEvent()                      reactor 调用 completion_handler
    → onMessage(conn, buf, time)                → handle.resume()
      → 上下文: 参数 + 全局 map                   → 上下文: 协程帧中的局部变量
      → 需要手动从 map 找 buffer                   → 变量就在帧里，直接用
```

两者在 epoll 层面做的是同一件事。区别在于：
- muduo 调用的是用户写的回调函数，通过参数和全局变量传递上下文
- asio 调用的是内部生成的 completion_handler 闭包，通过 coroutine_handle 跳回协程，上下文在协程帧上

coroutine_handle 就是 asio 找回"协程暂停点"的全部机制——一个指向堆上协程帧的指针，取出 `__state`，`switch` 跳回去，变量都在帧上。
