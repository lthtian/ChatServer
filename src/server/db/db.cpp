#include "db.h"
#include <muduo/base/Logging.h>
#include <vector>

static string server = "82.156.254.74";
static string user = "lth";
static string password = "040915ly";
static string dbname = "chat";

MySQL::MySQL()
{
    _conn = mysql_init(nullptr);
}
MySQL::~MySQL()
{
    if (_conn != nullptr)
        mysql_close(_conn);
}

bool MySQL::connect()
{
    MYSQL *p = mysql_real_connect(_conn, server.c_str(), user.c_str(), password.c_str(), dbname.c_str(), 3306, nullptr, 0);
    if (p != nullptr)
        mysql_query(_conn, "set names utf8mb4"); // 后面的选项是为了支持中文
    return p;
}

bool MySQL::update(string sql)
{
    if (mysql_query(_conn, sql.c_str()))
    {
        LOG_INFO << __FILE__ << ":" << __LINE__ << ":" << sql << "更新失败!";
        LOG_INFO << mysql_error(_conn);
        return false;
    }
    return true;
}

MYSQL_RES *MySQL::query(string sql)
{
    if (mysql_query(_conn, sql.c_str()))
    {
        LOG_INFO << __FILE__ << ":" << __LINE__ << ":" << sql << "查询失败!";
        LOG_INFO << mysql_error(_conn);
        return nullptr;
    }

    MYSQL_RES *res = mysql_store_result(_conn); // 这里改成 mysql_store_result
    if (!res && mysql_field_count(_conn) > 0)   // 只有 SELECT 语句应该返回结果集
    {
        LOG_INFO << "查询结果获取失败: " << mysql_error(_conn);
    }
    return res;
}

int MySQL::affected_rows()
{
    return mysql_affected_rows(_conn);
}

int MySQL::insert_image(const std::string &image_data)
{
    // 使用预处理语句防止SQL注入
    MYSQL_STMT *stmt = mysql_stmt_init(_conn);
    const char *sql = "INSERT INTO images (image_data) VALUES (?)";
    mysql_stmt_prepare(stmt, sql, strlen(sql));

    // 绑定参数
    MYSQL_BIND bind[1];
    memset(bind, 0, sizeof(bind));
    bind[0].buffer_type = MYSQL_TYPE_BLOB;
    bind[0].buffer = (void *)image_data.data();
    bind[0].buffer_length = image_data.size();

    mysql_stmt_bind_param(stmt, bind);
    mysql_stmt_execute(stmt);

    // 获取插入的ID
    int image_id = mysql_stmt_insert_id(stmt);
    mysql_stmt_close(stmt);
    return image_id;
}

std::string MySQL::get_image(int image_id)
{
    MYSQL_STMT *stmt = mysql_stmt_init(_conn);
    const char *sql = "SELECT image_data FROM images WHERE id=?";
    mysql_stmt_prepare(stmt, sql, strlen(sql));

    // 绑定参数
    MYSQL_BIND param, result;
    memset(&param, 0, sizeof(param));
    memset(&result, 0, sizeof(result));

    param.buffer_type = MYSQL_TYPE_LONG;
    param.buffer = &image_id;
    mysql_stmt_bind_param(stmt, &param);

    // 准备结果缓冲区
    std::vector<char> buffer(10 * 1024 * 1024); // 10MB预分配
    unsigned long data_length;
    result.buffer_type = MYSQL_TYPE_BLOB;
    result.buffer = buffer.data();
    result.buffer_length = buffer.size();
    result.length = &data_length;

    mysql_stmt_bind_result(stmt, &result);
    mysql_stmt_execute(stmt);
    mysql_stmt_fetch(stmt);

    std::string image_data(buffer.data(), data_length);
    mysql_stmt_close(stmt);
    return image_data;
}

bool MySQL::delete_image(int image_id)
{
    std::string sql = "DELETE FROM images WHERE id=" + std::to_string(image_id);
    return update(sql);
}