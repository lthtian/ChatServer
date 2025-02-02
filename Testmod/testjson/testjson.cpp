#include "json.hpp"
using json = nlohmann::json;

#include <iostream>
#include <vector>
#include <map>
#include <string>
using namespace std;

string test()
{
    json js;

    js["key"] = "value";

    js["arr"] = {1, 2, 3, 4, 5};
    js["msg"]["zhang san"] = "hello 1";
    js["msg"]["li si"] = "hello 2";

    vector<int> vec;
    vec.push_back(1);
    vec.push_back(2);
    js["vec"] = vec;

    map<int, string> m;
    m[1] = "黄山";
    m[2] = "华山";
    js["map"] = m;

    string sendBuf = js.dump(); // 转为string输出
    cout << sendBuf.c_str() << endl;

    return sendBuf;
}

int main()
{
    string recvBuf = test();
    json jsbuf = json::parse(recvBuf);
    cout << jsbuf["key"] << endl;

    auto arr = jsbuf["arr"];
    cout << arr[2] << endl;

    auto msgjs = jsbuf["msg"];
    cout << msgjs["zhang san"] << endl;

    vector<int> vec = jsbuf["vec"];
    cout << vec[1] << endl;

    map<int, string> m = jsbuf["map"];
    cout << m[2] << endl;
    return 0;
}