#include "chatserver.hpp"
#include "chatservice.hpp"
#include <iostream>
#include <signal.h>
using namespace std;

void resetHandler(int)
{
    ChatService::instance()->reset();
    exit(0);
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        printf("Usage: %s ip port\n", argv[0]);
        exit(0);
    }

    // 设置信号捕捉, 当异常退出时进行重置
    signal(SIGINT, resetHandler);
    signal(SIGSEGV, resetHandler);
    signal(SIGABRT, resetHandler);

    char *ip = argv[1];
    uint16_t port = atoi(argv[2]);

    EventLoop loop;
    InetAddress addr(ip, port);

    ChatServer cs(&loop, addr, "chat");
    cs.start();
    loop.loop();
    return 0;
}