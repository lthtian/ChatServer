#pragma once

// 用户数据访问层

#include "user.hpp"
#include "async_connectionpool.hpp"
#include <boost/asio.hpp>
#include <boost/mysql.hpp>

namespace asio = boost::asio;
namespace mysql = boost::mysql;

class UserModel
{
public:
    // 将用户注册信息插入数据库
    asio::awaitable<bool> insert(User &user)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return false;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "INSERT INTO User(name, password, state) VALUES(?, ?, ?)",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(
            stmt.bind(user.getName(), user.getPwd(), user.getState()),
            result, asio::use_awaitable);

        if (result.affected_rows() > 0)
        {
            // 设置对应的主键
            user.setId(static_cast<int>(result.last_insert_id()));
            co_return true;
        }
        co_return false;
    }

    asio::awaitable<bool> update(User &user)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return false;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "UPDATE User SET name=?, password=?, state=? WHERE id=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(
            stmt.bind(user.getName(), user.getPwd(), user.getState(), user.getId()),
            result, asio::use_awaitable);

        co_return result.affected_rows() > 0;
    }

    asio::awaitable<bool> updateState(User &user)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return false;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "UPDATE User SET state=? WHERE id=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(
            stmt.bind(user.getState(), user.getId()),
            result, asio::use_awaitable);

        co_return result.affected_rows() > 0;
    }

    asio::awaitable<bool> resetState()
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return false;

        auto& conn = guard->connection();

        mysql::results result;
        co_await conn.async_execute(
            "UPDATE User SET state='offline' WHERE state='online'",
            result, asio::use_awaitable);

        co_return result.affected_rows() > 0;
    }

    // 根据用户名称查询用户信息
    asio::awaitable<User> query(const string &name)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return User();

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "SELECT id, name, password, state FROM User WHERE name=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(name), result, asio::use_awaitable);

        auto rows = result.rows();
        if (!rows.empty())
        {
            const auto& row = rows[0];
            User user;
            user.setId(static_cast<int>(row[0].as_int64()));
            user.setName(std::string(row[1].as_string()));
            user.setPwd(std::string(row[2].as_string()));
            user.setState(std::string(row[3].as_string()));
            co_return user;
        }
        co_return User();
    }

    // 根据用户id返回用户状态
    asio::awaitable<string> queryState(const int id)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return string();

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "SELECT state FROM User WHERE id=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(id), result, asio::use_awaitable);

        auto rows = result.rows();
        if (!rows.empty())
        {
            co_return std::string(rows[0][0].as_string());
        }
        co_return string();
    }
};
