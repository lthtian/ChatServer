#include "async_connectionpool.hpp"
#include "log.hpp"
#include <iostream>

AsyncConnectionPool* AsyncConnectionPool::instance()
{
    // 静态局部变量，C++11保证线程安全的单例初始化
    static AsyncConnectionPool pool;
    return &pool;
}

AsyncConnectionPool::~AsyncConnectionPool()
{
    // 关闭所有连接
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pool_.empty())
    {
        auto conn = pool_.front();
        pool_.pop();
        if (conn)
        {
            boost::system::error_code ec;
            conn->close();
        }
    }
}

asio::awaitable<void> AsyncConnectionPool::init(asio::any_io_executor executor,
                                                   const DBConfig& config,
                                                   int connectionCount)
{
    if (initialized_)
    {
        co_return;
    }

    executor_ = std::move(executor);
    config_ = config;

    for (int i = 0; i < connectionCount; ++i)
    {
        auto conn = co_await create_connection();
        if (conn)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pool_.push(std::move(conn));
        }
        else
        {
            LOG_ERROR << "Failed to create MySQL connection " << i;
        }
    }

    initialized_ = true;
    LOG_INFO << "AsyncConnectionPool initialized with " << getAvailableCount() << " connections.";
}

asio::awaitable<std::shared_ptr<mysql::tcp_connection>> AsyncConnectionPool::create_connection()
{
    try
    {
        auto conn = std::make_shared<mysql::tcp_connection>(executor_);

        mysql::handshake_params params(config_.user, config_.password, config_.dbname);
        auto endpoint = tcp::endpoint(asio::ip::make_address(config_.server), config_.port);

        co_await conn->async_connect(endpoint, params, asio::use_awaitable);

        LOG_INFO << "MySQL connection created successfully to " << config_.server;
        co_return conn;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "Failed to connect to MySQL server: " << config_.server
                  << ", user: " << config_.user
                  << ", dbname: " << config_.dbname
                  << ", port: " << config_.port
                  << ", error: " << e.what();
        co_return nullptr;
    }
}

asio::awaitable<std::shared_ptr<mysql::tcp_connection>> AsyncConnectionPool::get_connection()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pool_.empty())
        {
            auto conn = pool_.front();
            pool_.pop();
            co_return conn;
        }
    }

    // 池空，等待连接被归还
    // 创建一个定时器用于挂起当前协程
    auto timer = std::make_shared<asio::steady_timer>(
        co_await asio::this_coro::executor,
        std::chrono::seconds(5));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        waiters_.push(timer);
    }

    // 挂起协程，等待连接被归还时 timer->cancel() 唤醒
    boost::system::error_code ec;
    co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));

    if (ec == asio::error::operation_aborted)
    {
        // 被唤醒，连接已归还到池中
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pool_.empty())
        {
            auto conn = pool_.front();
            pool_.pop();
            co_return conn;
        }
        // 如果连接已被其他协程取走，返回 nullptr
        LOG_ERROR << "getConnection: connection taken by another waiter";
        co_return nullptr;
    }

    // 超时
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 移除超时的 waiter（注意：如果这不是队首的 waiter，可能会出问题）
        // 简化处理：让 return_connection 的 cancel 自然跳过已完成的 timer
    }
    LOG_ERROR << "getConnection timeout, no available connection";
    co_return nullptr;
}

void AsyncConnectionPool::return_connection(std::shared_ptr<mysql::tcp_connection> conn)
{
    if (!conn)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!waiters_.empty())
    {
        // 有等待者，唤醒队首的一个
        auto timer = waiters_.front();
        waiters_.pop();
        pool_.push(std::move(conn));
        timer->cancel();
    }
    else
    {
        pool_.push(std::move(conn));
    }
}

int AsyncConnectionPool::getAvailableCount()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pool_.size();
}
