#pragma once

// 未读消息计数数据访问层

#include <iostream>
#include <vector>
#include "db.h"
#include "connectionpool.h"
using namespace std;

class NewMsgModel
{
public:
    // 增加未读消息计数
    void addNewMsgByKey(string key)
    {
        string sql = "select cnt from NewMsgCnt where `key` = '" + key + "';";

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql)
        {
            MYSQL_RES *result = mysql->query(sql);
            if (result)
            {
                int row_count = mysql_num_rows(result);
                cout << "row_count: " << row_count << endl;
                mysql_free_result(result);

                if (row_count == 0)
                {
                    sql = "insert into NewMsgCnt values('" + key + "', 1);";
                }
                else
                {
                    sql = "update NewMsgCnt set cnt = cnt + 1 where `key` = '" + key + "';";
                }
                mysql->update(sql);
            }
        }
    }

    // 删除未读消息计数
    void removeNewMsgByKey(string key)
    {
        string sql = "delete from NewMsgCnt where `key` = '" + key + "';";

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql)
        {
            if (!mysql->update(sql))
            {
                cerr << "删除失败: " << sql << endl;
            }
        }
    }

    // 获取未读消息计数
    int getNewMsgCntByKey(string key)
    {
        string sql = "select cnt from NewMsgCnt where `key` = '" + key + "';";
        cout << sql << endl;

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql)
        {
            MYSQL_RES *result = mysql->query(sql);
            if (result)
            {
                int cnt = 0;
                if (mysql_num_rows(result) == 0)
                {
                    mysql_free_result(result);
                    return 0;
                }
                else
                {
                    MYSQL_ROW row = mysql->fetch_row(result);
                    if (row && row[0])
                    {
                        cnt = stoi(row[0]);
                    }
                    mysql_free_result(result);
                    return cnt;
                }
            }
        }
        return 0;
    }
};