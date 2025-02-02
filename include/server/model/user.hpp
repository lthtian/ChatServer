#pragma once
#include <string>
using namespace std;

class User
{
public:
    User(int i = -1, const string &n = "", const string &p = "", const string &s = "offline")
        : id(i), name(n), password(p), state(s)
    {
    }

    void setId(int i) { id = i; }
    void setName(string n) { name = n; }
    void setPwd(string p) { password = p; }
    void setState(string s) { state = s; }

    int getId() { return id; }
    string getName() { return name; }
    string getPwd() { return password; }
    string getState() { return state; }

private:
    int id;
    string name;
    string password;
    string state;
};