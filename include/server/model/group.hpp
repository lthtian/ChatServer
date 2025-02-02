#pragma once

#include "groupuser.hpp"
#include <iostream>
#include <vector>
using namespace std;

class Group
{
public:
    Group(string name, string desc) : name(name), desc(desc) {}

    int getId() { return id; }
    string getName() { return name; }
    string getDesc() { return desc; }
    vector<GroupUser> &getUsers() { return users; }

    void setId(int id) { this->id = id; }
    void setName(string name) { this->name = name; }
    void setDesc(string desc) { this->desc = desc; }

private:
    int id = -1;
    string name;
    string desc;
    vector<GroupUser> users;
};