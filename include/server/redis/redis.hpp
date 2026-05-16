#ifndef REDIS_H
#define REDIS_H

#include <hiredis/async.h>
#include <hiredis/hiredis.h>
#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <string>

namespace asio = boost::asio;

using namespace std;

/*
 * Redis 客户端：publish 用同步连接，subscribe 用异步连接 + asio 集成
 * Redis 协议要求：进入 subscriber 模式的连接只能执行 SUBSCRIBE/UNSUBSCRIBE/PING/QUIT
 * 因此 publish 必须使用独立连接
 */
class Redis
{
public:
    Redis();
    ~Redis();

    // 异步连接 Redis 服务器（同时建立 publish 和 subscribe 两个连接）
    asio::awaitable<bool> connect(const string &host, uint16_t port);

    // 向 redis 指定通道发布消息（同步，fire-and-forget）
    void publish(int channel, const string &message);

    // 向 redis 指定通道订阅消息
    void subscribe(int channel);

    // 向 redis 指定通道取消订阅
    void unsubscribe(int channel);

    // 设置上报通道消息的回调函数
    void set_notify_handler(function<void(int, string)> fn);

private:
    // ===== subscribe 连接：异步上下文 + asio 集成 =====

    // hiredis 事件钩子（静态，被 hiredis 调用）
    static void ev_add_read(void *privdata);
    static void ev_del_read(void *privdata);
    static void ev_add_write(void *privdata);
    static void ev_del_write(void *privdata);
    static void ev_cleanup(void *privdata);

    // hiredis 连接/断开回调
    static void on_sub_connect(const redisAsyncContext *ac, int status);
    static void on_sub_disconnect(const redisAsyncContext *ac, int status);

    // hiredis 订阅消息回调
    static void on_subscribe_msg(redisAsyncContext *ac, void *r, void *privdata);

    // 内部方法：启动/停止读写监控
    void start_read();
    void stop_read();
    void start_write();
    void stop_write();

    // ===== 成员 =====

    // publish 专用：同步 hiredis 上下文（PUBLISH 是快速操作，同步不阻塞事件循环）
    redisContext *publish_ctx_ = nullptr;

    // subscribe 专用：异步 hiredis 上下文
    redisAsyncContext *sub_ctx_ = nullptr;

    // asio 流描述符（包装 subscribe 连接的 fd）
    unique_ptr<asio::posix::stream_descriptor> sd_;

    // 读写状态标志
    bool reading_ = false;
    bool writing_ = false;

    // 连接等待协程使用的信号（指向 connect() 栈上 timer）
    asio::steady_timer *connect_waiter_ = nullptr;
    bool connect_success_ = false;

    // 回调：收到订阅消息时上报给业务层
    function<void(int, string)> notify_handler_;
};

#endif
