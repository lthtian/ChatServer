#pragma once

#include <iostream>
#include <vector>
using namespace std;

#include "db.h"
#include "json.hpp"
using json = nlohmann::json;

class FriendModel
{
public:
    // 添加好友
    int insert(int userid, string name)
    {
        MySQL mysql;
        int friendid = -1;
        // 根据用户名查找用户id, 找不到返回1

        if (mysql.connect())
        {
            string sql1 = "select id from User where name = '" + name + "'";
            MYSQL_RES *result = mysql.query(sql1.c_str());
            if (result)
            {
                MYSQL_ROW row;
                row = mysql_fetch_row(result);
                if (row == nullptr)
                {
                    mysql.free(result);
                    return 1;
                }
                friendid = atoi(row[0]);
            }
            else
                return 1;

            mysql.free(result); // 释放结果集

            string sql2 = "insert into Friend(userid,friendid) values(" + to_string(userid) + "," + to_string(friendid) + ")";
            string sql3 = "insert into Friend(userid,friendid) values(" + to_string(friendid) + "," + to_string(userid) + ")";

            if (mysql.update(sql2) && mysql.update(sql3))
                return 0;
        }

        return 2;
    }

    void remove(int userid, int friendid)
    {
        string sql1 = "delete from Friend where userid = " + to_string(userid) + " and friendid = " + to_string(friendid);
        string sql2 = "delete from Friend where userid = " + to_string(friendid) + " and friendid = " + to_string(userid);
        MySQL mysql;
        if (mysql.connect())
        {
            mysql.update(sql1);
            mysql.update(sql2);
        }
    }
    // 根据用户id返回好友信息
    vector<string> query(int userid)
    {
        vector<string> ret; // 这里的string是好友信息的json, 包含id/name/state
        string sql = "select t1.friendid, t2.name, t2.state from Friend t1, User t2 where t1.friendid = t2.id and t1.userid = " + to_string(userid);

        MySQL mysql;
        if (mysql.connect())
        {
            MYSQL_RES *result = mysql.query(sql.c_str());
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
            }
        }

        return ret;
    }
};