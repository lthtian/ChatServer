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
    bool insert(int userid, int friendid)
    {
        string sql = "insert into Friend(userid,friendid) values(" + to_string(userid) + "," + to_string(friendid) + ")";

        MySQL mysql;
        if (mysql.connect())
        {
            if (mysql.update(sql))
                return true;
        }
        return false;
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