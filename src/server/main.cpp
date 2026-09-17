// ABOUTME: Starts the chat server and its configured database and media services.
// ABOUTME: Coordinates asynchronous startup and process shutdown.
#include "chatserver.hpp"
#include "chatservice.hpp"
#include "async_connectionpool.hpp"
#include "media_service.hpp"
#include <boost/asio.hpp>
#include <iostream>
#include <cstdlib>
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
asio::awaitable<void> bootstrap(ChatServer &server, chat::media::MediaService& media, const string &ip, int port)
{
    // 初始化数据库连接池
    DBConfig dbConfig;
    dbConfig.server = "127.0.0.1";
    dbConfig.user = "lth";
    dbConfig.password = "040915lLth!";
    dbConfig.dbname = "chat";
    dbConfig.port = 3306;
    if (const char* value = std::getenv("CHAT_DB_USER")) dbConfig.user = value;
    if (const char* value = std::getenv("CHAT_DB_PASSWORD")) dbConfig.password = value;
    if (const char* value = std::getenv("CHAT_DB_NAME")) dbConfig.dbname = value;

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
    ChatService::instance()->setMediaService(&media);
    media.start();
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
    signal(SIGTERM, resetHandler);

    asio::io_context ioc;
    g_ioc = &ioc;

    // 在 main 栈上创建服务器（生命周期由 main 管理）
    ChatServer server(ioc, argv[1], std::atoi(argv[2]));
    const char* root = std::getenv("CHAT_MEDIA_ROOT");
    const char* media_port = std::getenv("CHAT_MEDIA_PORT");
    const char* media_url = std::getenv("CHAT_MEDIA_URL");
    const int file_port = media_port ? std::stoi(media_port) : 6001;
    chat::media::MediaService media(ioc.get_executor(), root ? root : "media-data", file_port,
        media_url ? media_url : "http://127.0.0.1:" + std::to_string(file_port));

    // 初始化 + 启动 合并在同一个协程中，单次 ioc.run() 运行所有内容
    bool startup_failed = false;
    asio::co_spawn(ioc, bootstrap(server, media, argv[1], std::atoi(argv[2])),
        [&](std::exception_ptr error) {
            if (error) {
                startup_failed = true;
                try { std::rethrow_exception(error); }
                catch (const std::exception& failure) { cerr << "[SERVER] Startup failed: " << failure.what() << endl; }
                ioc.stop();
            }
        });
    ioc.run();
    server.stop();
    media.stop();
    ChatService::instance()->stop();

    // 服务器停止后，重置所有用户状态为 offline
    cout << "[SERVER] Resetting all users to offline..." << endl;
    ioc.restart();
    asio::co_spawn(ioc, ChatService::instance()->reset(), [&](std::exception_ptr error) {
        if (error) cerr << "[SERVER] Presence reset failed" << endl;
    });
    ioc.run();
    AsyncConnectionPool::instance()->close();
    g_ioc = nullptr;

    return startup_failed ? 1 : 0;
}
