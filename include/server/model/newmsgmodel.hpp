#pragma once
#include <iostream>
#include <vector>
#include "db.h"
using namespace std;

class NewMsgModel
{
public:
    void addNewMsgByKey(string key)
    {
        string sql = "select cnt from NewMsgCnt where `key` = '" + key + "';";
        MySQL mysql;
        if (mysql.connect())
        {
            MYSQL_RES *result = mysql.query(sql);
            if (result)
            {
                int row_count = mysql_num_rows(result);
                cout << "row_count: " << row_count << endl;
                mysql_free_result(result); // 释放结果，防止MySQL命令不同步
                if (row_count == 0)
                {
                    sql = "insert into NewMsgCnt values('" + key + "', 1);";
                }
                else
                {
                    sql = "update NewMsgCnt set cnt = cnt + 1 where `key` = '" + key + "';";
                }
                mysql.update(sql);
            }
        }
    }
    void removeNewMsgByKey(string key)
    {
        string sql = "delete from NewMsgCnt where `key` = '" + key + "';";
        MySQL mysql;
        if (mysql.connect())
        {
            if (!mysql.update(sql))
            {
                cerr << "删除失败: " << sql << endl;
            }
        }
    }

    int getNewMsgCntByKey(string key)
    {
        string sql = "select cnt from NewMsgCnt where `key` = '" + key + "';";
        cout << sql << endl;
        MySQL mysql;
        if (mysql.connect())
        {
            MYSQL_RES *result = mysql.query(sql);
            if (result)
            {
                int cnt = 0;
                if (mysql_num_rows(result) == 0)
                {
                    mysql_free_result(result); // 无数据也要释放
                    return 0;
                }
                else
                {
                    MYSQL_ROW row = mysql.fetch_row(result);
                    if (row && row[0]) // 检查 row[0] 是否有效
                    {
                        cnt = stoi(row[0]);
                    }
                    mysql_free_result(result); // 释放结果集
                    return cnt;
                }
            }
        }
        return 0;
    }
};