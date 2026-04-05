#pragma once

// 群组数据访问层

#include "group.hpp"
#include "db.h"
#include "connectionpool.h"

class GroupModel
{
public:
    // 创建群组
    int create(Group &group, int creatorid)
    {
        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (!mysql)
            return -1;

        // 先创建组
        string sql1 = "insert into AllGroup(groupname) values('" + group.getName() + "');";
        if (!mysql->update(sql1))
            return -1;

        int ret = mysql_insert_id(mysql->get_conn());
        group.setId(ret);

        // 再把创建者加入组
        string sql2 = "insert into GroupUser(userid, groupid, grouprole) values(" + to_string(creatorid) + ", " + to_string(group.getId()) + ", 'creator');";
        if (!mysql->update(sql2))
            return -1;

        return ret;
    }

    int queryGroupidByName(string groupname)
    {
        string sql = "select id from AllGroup where groupname = '" + groupname + "';";

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql)
        {
            MYSQL_RES *result = mysql->query(sql);
            if (result)
            {
                MYSQL_ROW row = mysql_fetch_row(result);
                if (row && row[0])
                {
                    int ret = stoi(row[0]);
                    mysql->free(result);
                    return ret;
                }
                mysql->free(result);
            }
        }
        return -1;
    }

    // 加入群组
    bool addTo(int userid, int groupid, string role)
    {
        string sql = "insert into GroupUser(userid, groupid, grouprole) values(" + to_string(userid) + ", " + to_string(groupid) + ", '" + role + "');";

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql)
            return mysql->update(sql);
        return false;
    }

    // 查询用户所在的群组信息
    vector<string> queryGroups(int userid)
    {
        vector<string> ret;
        string sql = "select t2.id, t2.groupname from GroupUser t1, AllGroup t2 where t1.userid = " + to_string(userid) + " and t1.groupid = t2.id;";

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql)
        {
            MYSQL_RES *result = mysql->query(sql);
            if (result)
            {
                MYSQL_ROW row;
                while (row = mysql_fetch_row(result))
                {
                    json js;
                    js["id"] = row[0];
                    js["groupname"] = row[1];
                    ret.push_back(js.dump());
                }
                mysql->free(result);
            }
        }
        return ret;
    }

    // 查询一个群组内除自己外的所有用户id
    vector<int> queryGroupUsersById(int groupid, int userid)
    {
        string sql = "select userid from GroupUser where groupid = " + to_string(groupid) + " and userid != " + to_string(userid);
        vector<int> users;

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql)
        {
            MYSQL_RES *result = mysql->query(sql);
            if (result)
            {
                MYSQL_ROW row;
                while (row = mysql_fetch_row(result))
                {
                    users.push_back(stoi(row[0]));
                }
                mysql->free(result);
            }
        }
        return users;
    }

    bool queryRoleById(int groupid, int userid)
    {
        string sql = "select grouprole from GroupUser where groupid = " + to_string(groupid) + " and userid = " + to_string(userid);

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql)
        {
            MYSQL_RES *result = mysql->query(sql);
            if (result)
            {
                MYSQL_ROW row = mysql_fetch_row(result);
                if (row && row[0])
                {
                    if (strcmp(row[0], "creator") == 0)
                    {
                        mysql->free(result);
                        return true;
                    }
                }
                mysql->free(result);
            }
        }
        return false;
    }

    void removeGroupById(int gid)
    {
        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (!mysql)
            return;

        // 在AllGroup中删除该群组
        string sql1 = "delete from AllGroup where id = " + to_string(gid);
        mysql->query(sql1);

        // 在GroupUser中删除该群组成员
        string sql2 = "delete from GroupUser where groupid = " + to_string(gid);
        mysql->query(sql2);
    }

    void removeUserFromGroup(int userid, int groupid)
    {
        string sql = "delete from GroupUser where userid = " + to_string(userid) + " and groupid = " + to_string(groupid);

        ConnectionGuard guard;
        MySQL* mysql = guard.get();
        if (mysql)
            mysql->query(sql);
    }
};