#pragma once

// MySQL数据库连接池

#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>

#include "db.h"

// 数据库连接配置
struct DBConfig {
    std::string server;    // 服务器地址
    std::string user;      // 用户名
    std::string password;  // 密码
    std::string dbname;    // 数据库名
    int port;              // 端口号
};

// MySQL连接池（单例模式）
class ConnectionPool {
public:
    // 获取单例实例
    static ConnectionPool* instance();

    // 初始化连接池
    void init(const DBConfig& config, int connectionCount);

    // 获取空闲连接（阻塞式）
    MySQL* getConnection();

    // 归还连接到池
    void releaseConnection(MySQL* conn);

    // 获取当前连接池状态
    int getAvailableCount();

private:
    ConnectionPool();
    ~ConnectionPool();

    // 创建新连接
    MySQL* createConnection();

    // 连接配置
    DBConfig _config;

    // 连接队列
    std::queue<MySQL*> _connQueue;

    // 线程同步
    std::mutex _mutex;
    std::condition_variable _cond;

    // 连接总数
    int _connectionCount;

    // 是否已初始化
    bool _initialized;
};

// RAII风格的连接Guard，自动获取和归还连接
class ConnectionGuard {
public:
    ConnectionGuard() : _conn(ConnectionPool::instance()->getConnection()) {}

    ~ConnectionGuard() {
        if (_conn) {
            ConnectionPool::instance()->releaseConnection(_conn);
        }
    }

    // 获取连接指针
    MySQL* get() { return _conn; }

    // 检查连接是否有效
    bool isValid() { return _conn != nullptr; }

private:
    MySQL* _conn;
};