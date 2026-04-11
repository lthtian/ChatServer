#include "connectionpool.h"
#include <muduo/base/Logging.h>
#include <iostream>

// 默认数据库配置
static DBConfig defaultConfig = {
    "39.105.18.142",  // server
    "lth",            // user
    "040915lLth!",       // password
    "chat",           // dbname
    3306              // port
};

ConnectionPool* ConnectionPool::instance()
{
    // 静态局部变量，C++11保证线程安全的单例初始化
    static ConnectionPool pool;
    return &pool;
}

ConnectionPool::ConnectionPool()
    : _connectionCount(0), _initialized(false)
{
}

ConnectionPool::~ConnectionPool()
{
    // 关闭所有连接
    std::lock_guard<std::mutex> lock(_mutex);
    while (!_connQueue.empty())
    {
        MySQL* conn = _connQueue.front();
        _connQueue.pop();
        if (conn)
        {
            conn->close_conn();
            delete conn;
        }
    }
}

void ConnectionPool::init(const DBConfig& config, int connectionCount)
{
    if (_initialized)
    {
        return;  // 已初始化，不重复初始化
    }

    _config = config;
    _connectionCount = connectionCount;

    std::lock_guard<std::mutex> lock(_mutex);

    // 创建指定数量的连接
    for (int i = 0; i < connectionCount; ++i)
    {
        MySQL* conn = createConnection();
        if (conn)
        {
            _connQueue.push(conn);
        }
        else
        {
            LOG_ERROR << "Failed to create MySQL connection " << i;
        }
    }

    _initialized = true;
    LOG_INFO << "ConnectionPool initialized with " << _connQueue.size() << " connections.";
}

MySQL* ConnectionPool::createConnection()
{
    MySQL* conn = new MySQL();
    // 使用配置建立连接
    if (conn->connect(_config.server, _config.user, _config.password, _config.dbname, _config.port))
    {
        return conn;
    }
    else
    {
        delete conn;
        return nullptr;
    }
}

MySQL* ConnectionPool::getConnection()
{
    std::unique_lock<std::mutex> lock(_mutex);

    // 等待有空闲连接
    while (_connQueue.empty())
    {
        // 如果队列空，等待条件变量通知
        if (_cond.wait_for(lock, std::chrono::milliseconds(5000)) == std::cv_status::timeout)
        {
            // 超时返回nullptr
            LOG_ERROR << "getConnection timeout, no available connection";
            return nullptr;
        }
    }

    MySQL* conn = _connQueue.front();
    _connQueue.pop();

    // 检查连接是否有效，无效则重新连接
    if (conn && !conn->isConnected())
    {
        LOG_INFO << "Connection invalid, reconnecting...";
        if (!conn->connect(_config.server, _config.user, _config.password, _config.dbname, _config.port))
        {
            // 重连失败，删除连接并返回nullptr
            delete conn;
            LOG_ERROR << "Reconnect failed";
            return nullptr;
        }
    }

    return conn;
}

void ConnectionPool::releaseConnection(MySQL* conn)
{
    if (!conn)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    _connQueue.push(conn);
    // 通知等待的线程
    _cond.notify_one();
}

int ConnectionPool::getAvailableCount()
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _connQueue.size();
}