#pragma once
#include <iostream>
#include <vector>
#include "db.h"
using namespace std;

class OfflineMsgModel
{
public:
    // 存储用户的离线消息
    void insert(const int &userid, const string &msg)
    {
        string sql = "insert into OfflineMessage(userid, message) values(" + to_string(userid) + ", '" + msg + "');";

        MySQL mysql;
        if (mysql.connect())
        {
            mysql.update(sql);
        }
    }

    // 根据用户id删除离线消息
    void remove(const int &userid)
    {
        string sql = "delete from OfflineMessage where userid = " + to_string(userid) + ";";

        MySQL mysql;
        if (mysql.connect())
        {
            mysql.update(sql);
        }
    }

    vector<string> query(int userid)
    {
        vector<string> vec;
        string sql = "select message from OfflineMessage where userid = " + to_string(userid) + ";";

        MySQL mysql;
        if (mysql.connect())
        {
            MYSQL_RES *res = mysql.query(sql);
            if (res != nullptr)
            {
                MYSQL_ROW row = mysql_fetch_row(res);
                while (row != nullptr)
                {
                    vec.push_back(row[0]);
                    row = mysql_fetch_row(res);
                }
                mysql_free_result(res);
            }
        }
        return vec;
    }
};