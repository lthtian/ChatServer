#include "redis.hpp"
#include "log.hpp"
#include <iostream>

Redis::Redis() = default;

Redis::~Redis()
{
    // 清理 subscribe 异步连接
    if (sd_ && sd_->is_open())
    {
        boost::system::error_code ec;
        sd_->cancel(ec);
        sd_->release(); // 不关闭 fd，hiredis 管理
        sd_.reset();
    }
    if (sub_ctx_)
    {
        redisAsyncFree(sub_ctx_);
        sub_ctx_ = nullptr;
    }

    // 清理 publish 同步连接
    if (publish_ctx_)
    {
        redisFree(publish_ctx_);
        publish_ctx_ = nullptr;
    }
}

// ============= 异步连接 =============

asio::awaitable<bool> Redis::connect(const string &host, uint16_t port)
{
    auto executor = co_await asio::this_coro::executor;

    // 1. 建立 publish 专用同步连接
    publish_ctx_ = redisConnect(host.c_str(), port);
    if (!publish_ctx_ || publish_ctx_->err)
    {
        LOG_ERROR << "[REDIS] Publish connect failed: "
                  << (publish_ctx_ ? publish_ctx_->errstr : "null context");
        if (publish_ctx_)
        {
            redisFree(publish_ctx_);
            publish_ctx_ = nullptr;
        }
        co_return false;
    }
    LOG_INFO << "[REDIS] Publish context connected";

    // 2. 建立 subscribe 异步连接
    sub_ctx_ = redisAsyncConnect(host.c_str(), port);
    if (!sub_ctx_ || sub_ctx_->err)
    {
        LOG_ERROR << "[REDIS] Subscribe connect failed immediately: "
                  << (sub_ctx_ ? sub_ctx_->errstr : "null context");
        if (sub_ctx_)
        {
            redisAsyncFree(sub_ctx_);
            sub_ctx_ = nullptr;
        }
        co_return false;
    }

    // 注册事件钩子（必须在 redisAsyncSetConnectCallback 之前）
    sub_ctx_->ev.data = this;
    sub_ctx_->ev.addRead = &Redis::ev_add_read;
    sub_ctx_->ev.delRead = &Redis::ev_del_read;
    sub_ctx_->ev.addWrite = &Redis::ev_add_write;
    sub_ctx_->ev.delWrite = &Redis::ev_del_write;
    sub_ctx_->ev.cleanup = &Redis::ev_cleanup;

    // 将 subscribe 连接的 fd 包装为 asio stream_descriptor
    sd_ = make_unique<asio::posix::stream_descriptor>(executor, sub_ctx_->c.fd);

    // 设置连接/断开回调
    redisAsyncSetConnectCallback(sub_ctx_, &Redis::on_sub_connect);
    redisAsyncSetDisconnectCallback(sub_ctx_, &Redis::on_sub_disconnect);

    // 挂起协程等待 subscribe 连接完成
    asio::steady_timer timer(executor);
    timer.expires_at(chrono::steady_clock::time_point::max());
    connect_waiter_ = &timer;
    connect_success_ = false;

    boost::system::error_code ec;
    co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));

    connect_waiter_ = nullptr;

    if (!connect_success_)
    {
        LOG_ERROR << "[REDIS] Subscribe async connect failed";
        co_return false;
    }

    LOG_INFO << "[REDIS] Async connect success!";
    co_return true;
}

// ============= pub/sub 操作 =============

void Redis::publish(int channel, const string &message)
{
    if (!publish_ctx_)
        return;
    // 使用独立的同步连接发送 PUBLISH（快速操作，不阻塞事件循环）
    redisReply *reply = (redisReply *)redisCommand(
        publish_ctx_, "PUBLISH %d %b", channel, message.data(), message.size());
    if (reply)
    {
        freeReplyObject(reply);
    }
}

void Redis::subscribe(int channel)
{
    if (!sub_ctx_)
        return;
    // 在 subscribe 异步连接上注册订阅
    redisAsyncCommand(sub_ctx_, &Redis::on_subscribe_msg, this,
                      "SUBSCRIBE %d", channel);
    LOG_INFO << "[REDIS] SUBSCRIBE " << channel;
}

void Redis::unsubscribe(int channel)
{
    if (!sub_ctx_)
        return;
    redisAsyncCommand(sub_ctx_, nullptr, nullptr, "UNSUBSCRIBE %d", channel);
    LOG_INFO << "[REDIS] UNSUBSCRIBE " << channel;
}

void Redis::set_notify_handler(function<void(int, string)> fn)
{
    notify_handler_ = std::move(fn);
}

// ============= hiredis 事件钩子 =============

void Redis::ev_add_read(void *privdata)
{
    static_cast<Redis *>(privdata)->start_read();
}

void Redis::ev_del_read(void *privdata)
{
    static_cast<Redis *>(privdata)->stop_read();
}

void Redis::ev_add_write(void *privdata)
{
    static_cast<Redis *>(privdata)->start_write();
}

void Redis::ev_del_write(void *privdata)
{
    static_cast<Redis *>(privdata)->stop_write();
}

void Redis::ev_cleanup(void *privdata)
{
    auto *self = static_cast<Redis *>(privdata);
    if (self->sd_ && self->sd_->is_open())
    {
        boost::system::error_code ec;
        self->sd_->cancel(ec);
    }
}

// ============= hiredis 回调 =============

void Redis::on_sub_connect(const redisAsyncContext *ac, int status)
{
    auto *self = static_cast<Redis *>(ac->ev.data);
    if (!self)
        return;

    self->connect_success_ = (status == REDIS_OK);
    if (status != REDIS_OK)
    {
        LOG_ERROR << "[REDIS] Subscribe connect callback error: " << ac->errstr;
    }

    if (self->connect_waiter_)
    {
        self->connect_waiter_->cancel();
    }
}

void Redis::on_sub_disconnect(const redisAsyncContext *ac, int status)
{
    if (status != REDIS_OK)
    {
        LOG_ERROR << "[REDIS] Subscribe disconnected: " << ac->errstr;
    }
    // 断连后标记 sub_ctx_ 为无效，防止后续操作
    // 注意：不能在这里 free，因为 hiredis 会在回调返回后继续使用上下文
}

void Redis::on_subscribe_msg(redisAsyncContext *ac, void *r, void *privdata)
{
    auto *self = static_cast<Redis *>(privdata);
    auto *reply = static_cast<redisReply *>(r);

    if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements < 3)
        return;

    // 只处理 "message" 类型（跳过 subscribe/unsubscribe 确认）
    if (strcmp(reply->element[0]->str, "message") == 0)
    {
        int channel = atoi(reply->element[1]->str);
        string data(reply->element[2]->str, reply->element[2]->len);
        if (self && self->notify_handler_)
        {
            self->notify_handler_(channel, std::move(data));
        }
    }
}

// ============= 内部读写管理 =============

void Redis::start_read()
{
    if (reading_ || !sd_ || !sd_->is_open())
        return;
    reading_ = true;

    sd_->async_wait(asio::posix::stream_descriptor::wait_read,
                    [this](boost::system::error_code ec)
                    {
                        reading_ = false;
                        if (!ec && sub_ctx_)
                        {
                            redisAsyncHandleRead(sub_ctx_);
                        }
                    });
}

void Redis::stop_read()
{
    reading_ = false;
}

void Redis::start_write()
{
    if (writing_ || !sd_ || !sd_->is_open())
        return;
    writing_ = true;

    sd_->async_wait(asio::posix::stream_descriptor::wait_write,
                    [this](boost::system::error_code ec)
                    {
                        writing_ = false;
                        if (!ec && sub_ctx_)
                        {
                            redisAsyncHandleWrite(sub_ctx_);
                        }
                    });
}

void Redis::stop_write()
{
    writing_ = false;
}
