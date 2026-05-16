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

// 异步初始化数据库连接池
asio::awaitable<void> init_pool()
{
    DBConfig dbConfig;
    dbConfig.server = "127.0.0.1";
    dbConfig.user = "lth";
    dbConfig.password = "040915lLth!";
    dbConfig.dbname = "chat";
    dbConfig.port = 3306;

    co_await AsyncConnectionPool::instance()->init(
        co_await asio::this_coro::executor, dbConfig, 10);

    cout << "[SERVER] Database connection pool initialized." << endl;
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

    // 第一步：异步初始化连接池（运行到完成后 ioc.run() 自动返回）
    asio::co_spawn(ioc, init_pool(), asio::detached);
    ioc.run();

    // 第二步：重启 ioc，创建服务器（在 main 栈上，生命周期正确）
    ioc.restart();
    ChatServer server(ioc, argv[1], std::atoi(argv[2]));
    server.start();

    cout << "[SERVER] ChatServer started on " << argv[1] << ":" << argv[2] << endl;

    // 运行服务器事件循环
    ioc.run();

    // 第三步：服务器停止后，重置所有用户状态为 offline
    cout << "[SERVER] Resetting all users to offline..." << endl;
    ioc.restart();
    ChatService::instance()->reset();
    ioc.run();

    return 0;
}
