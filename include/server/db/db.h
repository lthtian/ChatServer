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

    int affected_rows();
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

    void free(MYSQL_RES *res)
    {
        if (res != NULL)
            mysql_free_result(res);
    }

    MYSQL_ROW fetch_row(MYSQL_RES *result)
    {
        return mysql_fetch_row(result);
    }

    // 图片处理相关的函数
    int insert_image(const std::string &image_data);
    std::string get_image(int image_id);
    bool delete_image(int image_id);

private:
    MYSQL *_conn;
};
