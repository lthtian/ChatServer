#include "chatserver.hpp"
  #include "chatservice.hpp"
  #include <iostream>
  #include <signal.h>
  using namespace std;

  // 全局事件循环指针，用于信号处理
  EventLoop *g_loop = nullptr;

  void resetHandler(int)
  {
      cout << "\n[SERVER] Shutting down..." << endl;
      ChatService::instance()->reset();
      if (g_loop)
      {
          g_loop->quit();
      }
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
      g_loop = &loop;  // 保存循环指针
      InetAddress addr(ip, port);

      ChatServer cs(&loop, addr, "chat");
      cs.start();
      loop.loop();  // 收到 quit() 会正常退出
      return 0;
  }