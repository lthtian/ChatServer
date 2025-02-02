#pragma once

#include "user.hpp"

class GroupUser : public User
{
public:
    void setRole(std::string role)
    {
        this->role = role;
    }

private:
    std::string role; // 群内角色
};