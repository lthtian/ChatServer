#pragma once

// 异步MySQL连接池（基于boost::mysql）

#include <boost/asio.hpp>
#include <boost/mysql.hpp>
#include <string>
#include <queue>
#include <mutex>
#include <memory>
#include <optional>

namespace asio = boost::asio;
namespace mysql = boost::mysql;
using asio::ip::tcp;

// 数据库连接配置
struct DBConfig {
    std::string server;    // 服务器地址
    std::string user;      // 用户名
    std::string password;  // 密码
    std::string dbname;    // 数据库名
    int port;              // 端口号
};

// 异步MySQL连接池（单例模式）
// 使用 boost::mysql::tcp_connection 替代 mysqlclient C API
// 连接获取通过 co_await 挂起协程，不阻塞线程
class AsyncConnectionPool {
public:
    // 获取单例实例
    static AsyncConnectionPool* instance();

    // 异步初始化连接池
    asio::awaitable<void> init(asio::any_io_executor executor,
                                const DBConfig& config,
                                int connectionCount);

    // 异步获取空闲连接（无可用连接时挂起协程而非阻塞线程）
    asio::awaitable<std::shared_ptr<mysql::tcp_connection>> get_connection();

    // 归还连接到池
    void return_connection(std::shared_ptr<mysql::tcp_connection> conn);

    // 获取当前连接池状态
    int getAvailableCount();

    // 获取执行器（用于非协程上下文的 co_spawn fire-and-forget）
    asio::any_io_executor get_executor() { return executor_; }

private:
    AsyncConnectionPool() = default;
    ~AsyncConnectionPool();

    // 异步创建新连接
    asio::awaitable<std::shared_ptr<mysql::tcp_connection>> create_connection();

    // 执行器（用于创建新连接和定时器）
    asio::any_io_executor executor_;

    // 连接配置
    DBConfig config_;

    // 连接队列
    std::queue<std::shared_ptr<mysql::tcp_connection>> pool_;

    // 等待队列（每个等待者对应一个 timer）
    std::queue<std::shared_ptr<asio::steady_timer>> waiters_;

    // 线程同步（保护 pool_ 和 waiters_）
    std::mutex mutex_;

    // 是否已初始化
    bool initialized_ = false;
};

// RAII风格的异步连接Guard，自动获取和归还连接
// 使用工厂协程方法创建：auto guard = co_await AsyncConnectionGuard::create();
class AsyncConnectionGuard {
public:
    AsyncConnectionGuard() = delete;

    // 工厂方法：在协程中获取连接
    static asio::awaitable<std::unique_ptr<AsyncConnectionGuard>> create() {
        auto conn = co_await AsyncConnectionPool::instance()->get_connection();
        co_return std::unique_ptr<AsyncConnectionGuard>(new AsyncConnectionGuard(std::move(conn)));
    }

    ~AsyncConnectionGuard() {
        if (conn_) {
            AsyncConnectionPool::instance()->return_connection(std::move(conn_));
        }
    }

    // 获取连接指针
    mysql::tcp_connection* get() { return conn_.get(); }

    // 获取连接引用
    mysql::tcp_connection& connection() { return *conn_; }

    // 检查连接是否有效
    bool valid() const { return conn_ != nullptr; }

private:
    explicit AsyncConnectionGuard(std::shared_ptr<mysql::tcp_connection> conn)
        : conn_(std::move(conn)) {}

    std::shared_ptr<mysql::tcp_connection> conn_;
};
