#pragma once

// 聊天消息数据访问层

#include <iostream>
#include <vector>
#include "async_connectionpool.hpp"
#include "json.hpp"
#include <boost/asio.hpp>
#include <boost/mysql.hpp>
using namespace std;
using json = nlohmann::json;

namespace asio = boost::asio;
namespace mysql = boost::mysql;

class MessageModel
{
public:
    // 插入消息
    asio::awaitable<bool> insert(string chatkey, bool isgroup, int userid, string msg)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return false;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "INSERT INTO History(chatkey, userid, isgroup, message) VALUES(?, ?, ?, ?)",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(
            stmt.bind(chatkey, userid, isgroup ? 1 : 0, msg),
            result, asio::use_awaitable);

        co_return result.affected_rows() > 0;
    }

    // 根据chatkey查询消息
    asio::awaitable<vector<string>> query(string chatkey)
    {
        vector<string> res;

        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return res;

        auto& conn = guard->connection();

        // DATE_FORMAT 将 DATETIME 转为字符串，避免 boost::mysql 返回 datetime 对象
        auto stmt = co_await conn.async_prepare_statement(
            "SELECT t1.message, t1.userid, DATE_FORMAT(t1.time, '%Y-%m-%d %H:%i:%s'), t2.name "
            "FROM History t1, User t2 WHERE t1.chatkey=? AND t1.userid=t2.id ORDER BY t1.time ASC",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(chatkey), result, asio::use_awaitable);

        for (const auto& row : result.rows())
        {
            json js;
            js["message"] = std::string(row[0].as_string());
            js["id"] = std::to_string(static_cast<int>(row[1].as_int64()));
            js["time"] = std::string(row[2].as_string());
            js["name"] = std::string(row[3].as_string());
            res.push_back(js.dump());
        }

        co_return res;
    }

    // 根据chatkey删除消息
    asio::awaitable<bool> remove(string chatkey)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return false;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "DELETE FROM History WHERE chatkey=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(chatkey), result, asio::use_awaitable);

        co_return result.affected_rows() > 0;
    }
};
