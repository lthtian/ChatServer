#pragma once

// 未读消息计数数据访问层

#include <iostream>
#include <vector>
#include "async_connectionpool.hpp"
#include <boost/asio.hpp>
#include <boost/mysql.hpp>
using namespace std;

namespace asio = boost::asio;
namespace mysql = boost::mysql;

class NewMsgModel
{
public:
    // 增加未读消息计数
    asio::awaitable<void> addNewMsgByKey(string key)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return;

        auto& conn = guard->connection();

        // 先查询是否存在
        auto stmt1 = co_await conn.async_prepare_statement(
            "SELECT cnt FROM NewMsgCnt WHERE `key`=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt1.bind(key), result, asio::use_awaitable);

        if (result.rows().empty())
        {
            // 不存在，插入新记录
            auto stmt2 = co_await conn.async_prepare_statement(
                "INSERT INTO NewMsgCnt(`key`, cnt) VALUES(?, 1)",
                asio::use_awaitable);
            co_await conn.async_execute(stmt2.bind(key), result, asio::use_awaitable);
        }
        else
        {
            // 已存在，计数加1
            auto stmt2 = co_await conn.async_prepare_statement(
                "UPDATE NewMsgCnt SET cnt=cnt+1 WHERE `key`=?",
                asio::use_awaitable);
            co_await conn.async_execute(stmt2.bind(key), result, asio::use_awaitable);
        }
    }

    // 删除未读消息计数
    asio::awaitable<void> removeNewMsgByKey(string key)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "DELETE FROM NewMsgCnt WHERE `key`=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(key), result, asio::use_awaitable);
    }

    // 获取未读消息计数
    asio::awaitable<int> getNewMsgCntByKey(string key)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return 0;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "SELECT cnt FROM NewMsgCnt WHERE `key`=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(key), result, asio::use_awaitable);

        auto rows = result.rows();
        if (!rows.empty())
        {
            co_return static_cast<int>(rows[0][0].as_int64());
        }
        co_return 0;
    }
};
