#pragma once
#include <iostream>
#include <vector>
#include "db.h"
using namespace std;

class MessageModel
{
public:
    // 插入消息
    bool insert(string chatkey, bool isgroup, int userid, string msg)
    {
        string sql = "insert into History(chatkey,userid,isgroup,message) values('" + chatkey + "'," + to_string(userid) + "," + to_string(isgroup) + ",'" + msg + "');";
        MySQL mysql;
        if (mysql.connect())
        {
            if (mysql.update(sql))
                return true;
        }
        return false;
    }
    // 根据chatkey查询消息
    vector<string> query(string chatkey)
    {
        vector<string> res;
        string sql = "select t1.message, t1.userid, t1.time, t2.name from History t1, User t2 where t1.chatkey='" + chatkey + "' and t1.userid = t2.id ;";
        MySQL mysql;

        if (mysql.connect())
        {
            MYSQL_RES *result = mysql.query(sql.c_str());
            if (result)
            {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(result)))
                {
                    // 创建一个json对象
                    json js;
                    js["message"] = row[0]; // 第一个字段为 message
                    js["id"] = row[1];      // 第二个字段为 userid，对应 "id"
                    js["time"] = row[2];    // 第三个字段为 time
                    js["name"] = row[3];    // 第四个字段为 name
                    // 将json对象转换为字符串并添加到结果中
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
        MySQL mysql;
        if (mysql.connect())
        {
            if (mysql.update(sql))
                return true;
        }
        return false;
    }
};