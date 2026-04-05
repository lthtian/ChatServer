#pragma once

// 聊天消息数据访问层

#include <iostream>
#include <vector>
#include "db.h"
#include "connectionpool.h"
#include "json.hpp"
using namespace std;
using json = nlohmann::json;

class MessageModel
{
public:
    // 插入消息
    bool insert(string chatkey, bool isgroup, int userid, string msg)
    {
        string sql = "insert into History(chatkey,userid,isgroup,message) values('" + chatkey + "'," + to_string(userid) + "," + to_string(isgroup) + ",'" + msg + "');";

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql && mysql->update(sql))
            return true;
        return false;
    }

    // 根据chatkey查询消息
    vector<string> query(string chatkey)
    {
        vector<string> res;
        string sql = "select t1.message, t1.userid, t1.time, t2.name from History t1, User t2 where t1.chatkey='" + chatkey + "' and t1.userid = t2.id ;";

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql)
        {
            MYSQL_RES *result = mysql->query(sql.c_str());
            if (result)
            {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(result)))
                {
                    json js;
                    js["message"] = row[0];
                    js["id"] = row[1];
                    js["time"] = row[2];
                    js["name"] = row[3];
                    res.push_back(js.dump());
                }
                mysql_free_result(result);
            }
        }

        return res;
    }

    // 根据chatkey删除消息
    bool remove(string chatkey)
    {
        string sql = "delete from History where chatkey='" + chatkey + "';";

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql && mysql->update(sql))
            return true;
        return false;
    }
};