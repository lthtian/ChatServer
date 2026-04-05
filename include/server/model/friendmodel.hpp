#pragma once

// 好友关系数据访问层

#include <iostream>
#include <vector>
using namespace std;

#include "db.h"
#include "connectionpool.h"
#include "json.hpp"
using json = nlohmann::json;

class FriendModel
{
public:
    // 添加好友
    int insert(int userid, string name)
    {
        int friendid = -1;

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (!mysql)
            return 2;

        // 根据用户名查找用户id, 找不到返回1
        string sql1 = "select id from User where name = '" + name + "'";
        MYSQL_RES *result = mysql->query(sql1.c_str());
        if (result)
        {
            MYSQL_ROW row = mysql_fetch_row(result);
            if (row == nullptr)
            {
                mysql->free(result);
                return 1;
            }
            friendid = atoi(row[0]);
            mysql->free(result);
        }
        else
            return 1;

        // 双向添加好友关系
        string sql2 = "insert into Friend(userid,friendid) values(" + to_string(userid) + "," + to_string(friendid) + ")";
        string sql3 = "insert into Friend(userid,friendid) values(" + to_string(friendid) + "," + to_string(userid) + ")";

        if (mysql->update(sql2) && mysql->update(sql3))
            return 0;

        return 2;
    }

    void remove(int userid, int friendid)
    {
        string sql1 = "delete from Friend where userid = " + to_string(userid) + " and friendid = " + to_string(friendid);
        string sql2 = "delete from Friend where userid = " + to_string(friendid) + " and friendid = " + to_string(userid);

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql)
        {
            mysql->update(sql1);
            mysql->update(sql2);
        }
    }

    // 根据用户id返回好友信息
    vector<string> query(int userid)
    {
        vector<string> ret;
        string sql = "select t1.friendid, t2.name, t2.state from Friend t1, User t2 where t1.friendid = t2.id and t1.userid = " + to_string(userid);

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql)
        {
            MYSQL_RES *result = mysql->query(sql.c_str());
            if (result)
            {
                MYSQL_ROW row;
                while (row = mysql_fetch_row(result))
                {
                    json js;
                    js["id"] = row[0];
                    js["name"] = row[1];
                    js["state"] = row[2];
                    ret.push_back(js.dump());
                }
                mysql->free(result);
            }
        }

        return ret;
    }
};