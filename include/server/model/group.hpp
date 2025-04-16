#pragma once

#include "groupuser.hpp"
#include <iostream>
#include <vector>
using namespace std;

class Group
{
public:
    Group(string name) : name(name) {}

    int getId() { return id; }
    string getName() { return name; }
    vector<GroupUser> &getUsers() { return users; }

    void setId(int id) { this->id = id; }
    void setName(string name) { this->name = name; }

private:
    int id = -100;
    string name;
    vector<GroupUser> users;
};