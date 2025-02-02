#pragma once
#include <mysql/mysql.h>
#include <string>
using namespace std;

class MySQL
{
public:
    MySQL();
    ~MySQL();
    // 利用配置建立连接
    bool connect();
    // 更新语句
    bool update(string sql);
    // 查询语句
    MYSQL_RES *query(string sql);
    // 获取连接
    MYSQL *get_conn()
    {
        return _conn;
    }
    // 关闭连接
    void close_conn()
    {
        if (_conn != NULL)
            mysql_close(_conn), _conn = NULL;
    }

private:
    MYSQL *_conn;
};
