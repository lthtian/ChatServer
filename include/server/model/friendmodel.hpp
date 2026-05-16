#pragma once

// 好友关系数据访问层

#include <iostream>
#include <vector>
using namespace std;

#include "async_connectionpool.hpp"
#include "json.hpp"
#include <boost/asio.hpp>
#include <boost/mysql.hpp>
using json = nlohmann::json;

namespace asio = boost::asio;
namespace mysql = boost::mysql;

class FriendModel
{
public:
    // 添加好友
    asio::awaitable<int> insert(int userid, string name)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return 2;

        auto& conn = guard->connection();

        // 根据用户名查找用户id, 找不到返回1
        auto stmt1 = co_await conn.async_prepare_statement(
            "SELECT id FROM User WHERE name=?",
            asio::use_awaitable);

        mysql::results result1;
        co_await conn.async_execute(stmt1.bind(name), result1, asio::use_awaitable);

        auto rows1 = result1.rows();
        if (rows1.empty())
            co_return 1;

        int friendid = static_cast<int>(rows1[0][0].as_int64());

        // 双向添加好友关系
        auto stmt2 = co_await conn.async_prepare_statement(
            "INSERT INTO Friend(userid, friendid) VALUES(?, ?)",
            asio::use_awaitable);

        mysql::results result2;
        co_await conn.async_execute(stmt2.bind(userid, friendid), result2, asio::use_awaitable);

        co_await conn.async_execute(stmt2.bind(friendid, userid), result2, asio::use_awaitable);

        co_return 0;
    }

    asio::awaitable<void> remove(int userid, int friendid)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "DELETE FROM Friend WHERE userid=? AND friendid=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(userid, friendid), result, asio::use_awaitable);
        co_await conn.async_execute(stmt.bind(friendid, userid), result, asio::use_awaitable);
    }

    // 根据用户id返回好友信息
    asio::awaitable<vector<string>> query(int userid)
    {
        vector<string> ret;

        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return ret;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "SELECT t1.friendid, t2.name, t2.state FROM Friend t1, User t2 WHERE t1.friendid=t2.id AND t1.userid=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(userid), result, asio::use_awaitable);

        for (const auto& row : result.rows())
        {
            json js;
            js["id"] = std::to_string(static_cast<int>(row[0].as_int64()));
            js["name"] = std::string(row[1].as_string());
            js["state"] = std::string(row[2].as_string());
            ret.push_back(js.dump());
        }

        co_return ret;
    }
};
