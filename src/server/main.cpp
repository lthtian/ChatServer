#include "chatserver.hpp"
#include "chatservice.hpp"
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
    ChatService::instance()->reset();
    if (g_ioc)
    {
        g_ioc->stop();
    }
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

    ChatServer server(ioc, argv[1], std::atoi(argv[2]));
    server.start();

    // 运行事件循环
    ioc.run();

    return 0;
}
