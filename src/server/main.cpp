#include "chatserver.hpp"
#include "chatservice.hpp"
#include "async_connectionpool.hpp"
#include <boost/asio.hpp>
#include <iostream>
#include <signal.h>
using namespace std;

namespace asio = boost::asio;

// 全局事件循环指针，用于信号处理
asio::io_context *g_ioc = nullptr;

void resetHandler(int)
{
    cout << "\n[SERVER] Shutting down..." << endl;
    // 注意：信号处理器中不能调用 co_spawn（会 malloc）
    // 只做轻量操作：停止事件循环
    if (g_ioc)
    {
        g_ioc->stop();
    }
}

// 统一初始化 + 启动服务器（在同一个协程中完成）
// 解决：Redis 连接后 stream_descriptor 上有 pending async_wait，
// 导致第一阶段 ioc.run() 无法返回，服务器无法启动的问题
asio::awaitable<void> bootstrap(ChatServer &server, const string &ip, int port)
{
    // 初始化数据库连接池
    DBConfig dbConfig;
    dbConfig.server = "127.0.0.1";
    dbConfig.user = "lth";
    dbConfig.password = "040915lLth!";
    dbConfig.dbname = "chat";
    dbConfig.port = 3306;

    co_await AsyncConnectionPool::instance()->init(
        co_await asio::this_coro::executor, dbConfig, 10);

    cout << "[SERVER] Database connection pool initialized." << endl;

    // 异步初始化 Redis
    bool redis_ok = co_await ChatService::instance()->init_redis();
    if (redis_ok)
    {
        cout << "[SERVER] Redis connected." << endl;
    }
    else
    {
        cerr << "[SERVER] Redis connection failed!" << endl;
    }

    // 启动服务器（在同一个 ioc.run() 中运行）
    server.start();
    cout << "[SERVER] ChatServer started on " << ip << ":" << port << endl;
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        printf("Usage: %s ip port\n", argv[0]);
        return 1;
    }

    // 设置信号捕捉, 当异常退出时进行重置
    signal(SIGINT, resetHandler);
    signal(SIGSEGV, resetHandler);
    signal(SIGABRT, resetHandler);

    asio::io_context ioc;
    g_ioc = &ioc;

    // 在 main 栈上创建服务器（生命周期由 main 管理）
    ChatServer server(ioc, argv[1], std::atoi(argv[2]));

    // 初始化 + 启动 合并在同一个协程中，单次 ioc.run() 运行所有内容
    asio::co_spawn(ioc, bootstrap(server, argv[1], std::atoi(argv[2])), asio::detached);
    ioc.run();

    // 服务器停止后，重置所有用户状态为 offline
    cout << "[SERVER] Resetting all users to offline..." << endl;
    ioc.restart();
    ChatService::instance()->reset();
    ioc.run();

    return 0;
}
