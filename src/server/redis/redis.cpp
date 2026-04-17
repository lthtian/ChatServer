#include "redis.hpp"
  #include <iostream>
  #include <unistd.h>
  #include <pthread.h>
  using namespace std;

  Redis::Redis()
      : _publish_context(nullptr), _subcribe_context(nullptr), _stop_flag(false)
  {
  }

  Redis::~Redis()
  {
      _stop_flag = true;

      // 先尝试取消线程（必须在 join 之前）
      if (_subscriber_thread.joinable())
      {
          pthread_cancel(_subscriber_thread.native_handle());
      }

      // 强制关闭 socket
      if (_subcribe_context != nullptr && _subcribe_context->fd > 0)
      {
          close(_subcribe_context->fd);
          _subcribe_context->fd = -1;
      }

      // 现在 join 应该能立即返回
      if (_subscriber_thread.joinable())
      {
          _subscriber_thread.join();
      }

      if (_subcribe_context != nullptr)
      {
          redisFree(_subcribe_context);
          _subcribe_context = nullptr;
      }

      if (_publish_context != nullptr)
      {
          redisFree(_publish_context);
          _publish_context = nullptr;
      }
  }

  bool Redis::connect()
  {
      // 负责publish发布消息的上下文连接
      _publish_context = redisConnect("127.0.0.1", 6379);
      if (nullptr == _publish_context)
      {
          cerr << "connect redis failed!" << endl;
          return false;
      }

      // 负责subscribe订阅消息的上下文连接
      _subcribe_context = redisConnect("127.0.0.1", 6379);
      if (nullptr == _subcribe_context)
      {
          cerr << "connect redis failed!" << endl;
          return false;
      }

      // 在单独的线程中，监听通道上的事件，有消息给业务层进行上报
      _subscriber_thread = std::thread([this]()
                                       { observer_channel_message(); });

      cout << "connect redis-server success!" << endl;

      return true;
  }

  // 向redis指定的通道channel发布消息
  bool Redis::publish(int channel, string message)
  {
      redisReply *reply = (redisReply *)redisCommand(_publish_context, "PUBLISH %d %s", channel, message.c_str());
      if (nullptr == reply)
      {
          std::cerr << "[REDIS] PUBLISH failed to channel " << channel << std::endl;
          return false;
      }
      freeReplyObject(reply);
      std::cout << "[REDIS] PUBLISH to " << channel << std::endl;
      return true;
  }

  // 向redis指定的通道subscribe订阅消息
  bool Redis::subscribe(int channel)
  {
      // SUBSCRIBE命令本身会造成线程阻塞等待通道里面发生消息，这里只做订阅通道，不接收通道消息
      // 通道消息的接收专门在observer_channel_message函数中的独立线程中进行
      // 只负责发送命令，不阻塞接收redis server响应消息，否则和notifyMsg线程抢占响应资源
      if (REDIS_ERR == redisAppendCommand(this->_subcribe_context, "SUBSCRIBE %d", channel))
      {
          std::cerr << "[REDIS] SUBSCRIBE failed for channel " << channel << std::endl;
          return false;
      }
      // redisBufferWrite可以循环发送缓冲区，直到缓冲区数据发送完毕（done被置为1）
      int done = 0;
      while (!done)
      {
          if (REDIS_ERR == redisBufferWrite(this->_subcribe_context, &done))
          {
              std::cerr << "[REDIS] SUBSCRIBE write failed for channel " << channel << std::endl;
              return false;
          }
      }
      std::cout << "[REDIS] SUBSCRIBE " << channel << std::endl;
      return true;
  }

  // 向redis指定的通道unsubscribe取消订阅消息
  bool Redis::unsubscribe(int channel)
  {
      if (REDIS_ERR == redisAppendCommand(this->_subcribe_context, "UNSUBSCRIBE %d", channel))
      {
          std::cerr << "[REDIS] UNSUBSCRIBE failed for channel " << channel << std::endl;
          return false;
      }
      // redisBufferWrite可以循环发送缓冲区，直到缓冲区数据发送完毕（done被置为1）
      int done = 0;
      while (!done)
      {
          if (REDIS_ERR == redisBufferWrite(this->_subcribe_context, &done))
          {
              std::cerr << "[REDIS] UNSUBSCRIBE write failed for channel " << channel << std::endl;
              return false;
          }
      }
      std::cout << "[REDIS] UNSUBSCRIBE " << channel << std::endl;
      return true;
  }

  // 在独立线程中接收订阅通道中的消息
  void Redis::observer_channel_message()
  {
      std::cout << "[Redis] Observer thread started." << std::endl;
      redisReply *reply = nullptr;
      // 持续监听通道上是否有消息发生
      // 如果在其他服务器publish了本服务器订阅的通道, 则本服务器会收到消息
      // 进而调用回调函数进行业务处理, 本项目中的业务就是发送消息给对应id的客户
      while (!_stop_flag && REDIS_OK == redisGetReply(this->_subcribe_context, (void **)&reply))
      {
          // 订阅收到的消息是一个带三元素的数组
          if (reply != nullptr && reply->element[2] != nullptr && reply->element[2]->str != nullptr)
          {
              // 给业务层上报通道上发生的消息
              // 第一个参数是通道号, 第二个参数是通道消息
              _notify_message_handler(atoi(reply->element[1]->str), reply->element[2]->str);
          }

          freeReplyObject(reply);
      }
      std::cout << "[Redis] Observer thread exiting, stop_flag=" << _stop_flag << std::endl;
  }

  // 设置上报通道消息的回调函数
  void Redis::init_notify_handler(function<void(int, string)> fn)
  {
      this->_notify_message_handler = fn;
  }