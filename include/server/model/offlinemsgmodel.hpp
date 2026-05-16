#pragma once

// 离线消息数据访问层

#include <iostream>
#include <vector>
#include "async_connectionpool.hpp"
#include <boost/asio.hpp>
#include <boost/mysql.hpp>
using namespace std;

namespace asio = boost::asio;
namespace mysql = boost::mysql;

class OfflineMsgModel
{
public:
    // 存储用户的离线消息
    asio::awaitable<void> insert(const int &userid, const string &msg)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "INSERT INTO OfflineMessage(userid, message) VALUES(?, ?)",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(userid, msg), result, asio::use_awaitable);
    }

    // 根据用户id删除离线消息
    asio::awaitable<void> remove(const int &userid)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "DELETE FROM OfflineMessage WHERE userid=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(userid), result, asio::use_awaitable);
    }

    // 查询用户的离线消息
    asio::awaitable<vector<string>> query(int userid)
    {
        vector<string> vec;

        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return vec;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "SELECT message FROM OfflineMessage WHERE userid=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(userid), result, asio::use_awaitable);

        for (const auto& row : result.rows())
        {
            vec.push_back(std::string(row[0].as_string()));
        }
        co_return vec;
    }
};
