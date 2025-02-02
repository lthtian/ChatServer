#pragma once

#include "group.hpp"
#include "db.h"

class GroupModel
{
public:
    // 创建群组
    bool create(Group &group) // 半输出型参数
    {
        string sql = "insert into AllGroup(groupname, groupdesc) values('" + group.getName() + "', '" + group.getDesc() + "');";

        MySQL mysql;
        if (mysql.connect())
        {
            if (mysql.update(sql))
            {
                group.setId(mysql_insert_id(mysql.get_conn()));
                return true;
            }
        }
        return false;
    }
    // 加入群组
    bool addTo(int userid, int groupid, string role)
    {
        string sql = "insert into GroupUser(userid, groupid, grouprole) values(" + to_string(userid) + ", " + to_string(groupid) + ", '" + role + "');";

        MySQL mysql;
        if (mysql.connect())
        {
            return mysql.update(sql);
        }
        return false;
    }
    // 查询用户所在的群组信息
    vector<string> queryGroups(int userid) // 每个string为一个group的json
    {
        return {};
    }
    // 查询一个群组内除自己外的所有用户id
    vector<int> queryGroupUsersById(int groupid, int userid)
    {
        string sql = "select userid from GroupUser where groupid = " + to_string(groupid) + " and userid != " + to_string(userid);

        vector<int> users;

        MySQL mysql;
        if (mysql.connect())
        {
            MYSQL_RES *result = mysql.query(sql);
            if (result)
            {
                MYSQL_ROW row;
                while (row = mysql_fetch_row(result))
                {
                    users.push_back(stoi(row[0]));
                }
            }
        }
        return users;
    }
};