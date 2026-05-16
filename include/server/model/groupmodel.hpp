#pragma once

// 群组数据访问层

#include "group.hpp"
#include "async_connectionpool.hpp"
#include "json.hpp"
#include <boost/asio.hpp>
#include <boost/mysql.hpp>

namespace asio = boost::asio;
namespace mysql = boost::mysql;
using json = nlohmann::json;

class GroupModel
{
public:
    // 创建群组
    asio::awaitable<int> create(Group &group, int creatorid)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return -1;

        auto& conn = guard->connection();

        // 先创建组
        auto stmt1 = co_await conn.async_prepare_statement(
            "INSERT INTO AllGroup(groupname) VALUES(?)",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt1.bind(group.getName()), result, asio::use_awaitable);

        if (result.affected_rows() == 0)
            co_return -1;

        int ret = static_cast<int>(result.last_insert_id());
        group.setId(ret);

        // 再把创建者加入组
        auto stmt2 = co_await conn.async_prepare_statement(
            "INSERT INTO GroupUser(userid, groupid, grouprole) VALUES(?, ?, 'creator')",
            asio::use_awaitable);

        co_await conn.async_execute(stmt2.bind(creatorid, group.getId()), result, asio::use_awaitable);

        co_return ret;
    }

    asio::awaitable<int> queryGroupidByName(string groupname)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return -1;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "SELECT id FROM AllGroup WHERE groupname=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(groupname), result, asio::use_awaitable);

        auto rows = result.rows();
        if (!rows.empty())
        {
            co_return static_cast<int>(rows[0][0].as_int64());
        }
        co_return -1;
    }

    // 加入群组
    asio::awaitable<bool> addTo(int userid, int groupid, string role)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return false;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "INSERT INTO GroupUser(userid, groupid, grouprole) VALUES(?, ?, ?)",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(userid, groupid, role), result, asio::use_awaitable);

        co_return result.affected_rows() > 0;
    }

    // 查询用户所在的群组信息
    asio::awaitable<vector<string>> queryGroups(int userid)
    {
        vector<string> ret;

        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return ret;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "SELECT t2.id, t2.groupname FROM GroupUser t1, AllGroup t2 WHERE t1.userid=? AND t1.groupid=t2.id",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(userid), result, asio::use_awaitable);

        for (const auto& row : result.rows())
        {
            json js;
            js["id"] = std::to_string(static_cast<int>(row[0].as_int64()));
            js["groupname"] = std::string(row[1].as_string());
            ret.push_back(js.dump());
        }
        co_return ret;
    }

    // 查询一个群组内除自己外的所有用户id
    asio::awaitable<vector<int>> queryGroupUsersById(int groupid, int userid)
    {
        vector<int> users;

        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return users;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "SELECT userid FROM GroupUser WHERE groupid=? AND userid!=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(groupid, userid), result, asio::use_awaitable);

        for (const auto& row : result.rows())
        {
            users.push_back(static_cast<int>(row[0].as_int64()));
        }
        co_return users;
    }

    asio::awaitable<bool> queryRoleById(int groupid, int userid)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return false;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "SELECT grouprole FROM GroupUser WHERE groupid=? AND userid=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(groupid, userid), result, asio::use_awaitable);

        auto rows = result.rows();
        if (!rows.empty())
        {
            auto role = std::string(rows[0][0].as_string());
            co_return role == "creator";
        }
        co_return false;
    }

    asio::awaitable<void> removeGroupById(int gid)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return;

        auto& conn = guard->connection();

        mysql::results result;

        // 在AllGroup中删除该群组
        auto stmt1 = co_await conn.async_prepare_statement(
            "DELETE FROM AllGroup WHERE id=?",
            asio::use_awaitable);
        co_await conn.async_execute(stmt1.bind(gid), result, asio::use_awaitable);

        // 在GroupUser中删除该群组成员
        auto stmt2 = co_await conn.async_prepare_statement(
            "DELETE FROM GroupUser WHERE groupid=?",
            asio::use_awaitable);
        co_await conn.async_execute(stmt2.bind(gid), result, asio::use_awaitable);
    }

    asio::awaitable<void> removeUserFromGroup(int userid, int groupid)
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid())
            co_return;

        auto& conn = guard->connection();

        auto stmt = co_await conn.async_prepare_statement(
            "DELETE FROM GroupUser WHERE userid=? AND groupid=?",
            asio::use_awaitable);

        mysql::results result;
        co_await conn.async_execute(stmt.bind(userid, groupid), result, asio::use_awaitable);
    }
};
