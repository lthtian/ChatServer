#pragma once

// 用户数据访问层

#include "user.hpp"
#include "db.h"
#include "connectionpool.h"

class UserModel
{
public:
    // 将用户注册信息插入数据库
    bool insert(User &user)
    {
        string sql = "insert into User(name,password, state) values('" + user.getName() + "','" + user.getPwd() + "','" + user.getState() + "')";

        // 使用连接池获取连接，ConnectionGuard自动归还
        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql && mysql->update(sql))
        {
            // 设置对应的主键
            user.setId(mysql_insert_id(mysql->get_conn()));
            return true;
        }
        return false;
    }

    bool update(User &user)
    {
        string sql = "update User set name = '" + user.getName() + "', password = '" + user.getPwd() + "', state = '" + user.getState() + "' where id = " + to_string(user.getId());

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql && mysql->update(sql))
            return true;
        return false;
    }

    bool updateState(User &user)
    {
        string sql = "update User set state = '" + user.getState() + "' where id = " + to_string(user.getId());

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql && mysql->update(sql))
            return true;
        return false;
    }

    bool resetState()
    {
        string sql = "update User set state = 'offline' where state = 'online'";

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql && mysql->update(sql))
            return true;
        return false;
    }

    // 根据用户名称查询用户信息
    User query(const string &name)
    {
        string sql = "select * from User where name = '" + name + "'";

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql)
        {
            MYSQL_RES *res = mysql->query(sql);
            if (res != nullptr)
            {
                MYSQL_ROW row = mysql_fetch_row(res);
                if (row != nullptr)
                {
                    User user;
                    user.setId(atoi(row[0]));
                    user.setName(row[1]);
                    user.setPwd(row[2]);
                    user.setState(row[3]);
                    mysql_free_result(res);
                    return user;
                }
                mysql_free_result(res);
            }
        }
        return User();
    }

    // 根据用户id返回用户状态
    string queryState(const int id)
    {
        string sql = "select state from User where id = " + to_string(id);

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql)
        {
            MYSQL_RES *res = mysql->query(sql);
            if (res != nullptr)
            {
                MYSQL_ROW row = mysql_fetch_row(res);
                if (row != nullptr)
                {
                    string ret = row[0];
                    mysql_free_result(res);
                    return ret;
                }
                mysql_free_result(res);
            }
        }
        return string();
    }
};