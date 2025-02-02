#include "chatservice.hpp"
#include "public.hpp"
#include <muduo/base/Logging.h>
using namespace muduo;

ChatService *ChatService::instance()
{
    static ChatService service;
    return &service;
}

ChatService::ChatService()
{
    // 为每个消息类型注册对应的业务代码
    _mhm.insert({LoginMsg, std::bind(&ChatService::login, this, _1, _2, _3)});
    _mhm.insert({RegMsg, std::bind(&ChatService::reg, this, _1, _2, _3)});
    _mhm.insert({OTOMsg, std::bind(&ChatService::otoChat, this, _1, _2, _3)});
    _mhm.insert({AddFriendMsg, std::bind(&ChatService::addFriend, this, _1, _2, _3)});
    _mhm.insert({CreateGroupMsg, std::bind(&ChatService::createGroup, this, _1, _2, _3)});
    _mhm.insert({AddGroupMsg, std::bind(&ChatService::addGroup, this, _1, _2, _3)});
    _mhm.insert({GroupChatMsg, std::bind(&ChatService::groupChat, this, _1, _2, _3)});
    _mhm.insert({loginOutMsg, std::bind(&ChatService::loginout, this, _1, _2, _3)});

    // 注册redis服务并且绑定回调函数
    if (_redis.connect())
    {
        _redis.init_notify_handler(std::bind(&ChatService::handleRedisSubscribeMessage, this, _1, _2));
    }
}

// 如果收到publish的订阅消息, 会调用该回调函数, 向消息中对应的用户进行推送
void ChatService::handleRedisSubscribeMessage(int id, string msg)
{
    lock_guard<mutex> lock(_connMutex);
    auto it = _userConnMap.find(id);
    if (it != _userConnMap.end())
    {
        it->second->send(msg);
        return;
    }

    _offlineMsgModel.insert(id, msg);
}

// 获取消息类型对应的处理器
MsgHandler ChatService::getHandler(int msgid)
{
    auto it = _mhm.find(msgid);
    if (it == _mhm.end())
    {
        // 找不到就返回一个空的处理器, 该处理器可以返回提示信息
        return [=](const TcpConnectionPtr &conn, json &js, Timestamp time)
        {
            LOG_ERROR << "msgid:" << msgid << " can not find handler!";
        };
    }
    else
        return _mhm[msgid];
}

void ChatService::login(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    string name = js["username"];
    string password = js["password"];
    User user = _userModel.query(name);
    if (user.getName() == name && user.getPwd() == password)
    {
        if (user.getState() == "online")
        {
            // 该用户已经登录, 不能重复登录
            json response;
            response["msgid"] = LoginMsgAck;
            response["errmsg"] = "该用户已经登录, 不能重复登录";
            response["id"] = user.getId();
            response["errno"] = 1;
            conn->send(response.dump());
            return;
        }

        // 登录成功, 更新状态, 记录连接信息
        user.setState("online");
        _userModel.updateState(user);

        {
            lock_guard<mutex> lock(_connMutex);
            _userConnMap.insert({user.getId(), conn}); // 存在线程安全问题, 可能同时向map插入
        }

        // 登录成功订阅与该用户id相同的频道
        _redis.subscribe(user.getId());

        // 返回前端需要的数据
        json response;
        response["msgid"] = LoginMsgAck;
        response["errno"] = 0;
        response["id"] = user.getId();
        response["name"] = user.getName();

        // 查询离线消息, 有就传入
        vector<string> ret = _offlineMsgModel.query(user.getId());
        if (!ret.empty())
        {
            response["offlinemsg"] = ret;
            // 把已经查到的信息删除
            _offlineMsgModel.remove(user.getId());
        }

        // 查询好友列表
        vector<string> friends = _friendModel.query(user.getId());
        if (!friends.empty())
        {
            response["friends"] = friends;
        }

        conn->send(response.dump());
    }
    else
    {
        // 用户不存在或密码错误
        json response;
        response["msgid"] = LoginMsgAck;
        response["errno"] = 2; // 2
        response["errmsg"] = "用户不存在或密码错误";
        conn->send(response.dump());
    }
}

void ChatService::loginout(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();
    // 更新状态
    User user(userid, "", "", "offline");
    _userModel.updateState(user);

    // 删除连接信息
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(userid);
        if (it != _userConnMap.end())
            _userConnMap.erase(userid);
    }

    // 退订
    _redis.unsubscribe(userid);
}

// 注册任务逻辑 : 填入name, password, 插入到数据库中
void ChatService::reg(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    string name = js["name"];
    string password = js["password"];

    User user;
    user.setName(name);
    user.setPwd(password);
    bool ret = _userModel.insert(user);
    if (ret)
    {
        json response;
        response["msgid"] = RegMsgAck;
        response["errno"] = 0;
        response["id"] = user.getId();
        conn->send(response.dump());
    }
    else
    {
        json response;
        response["msgid"] = RegMsgAck;
        response["errno"] = 1;
        conn->send(response.dump());
    }
}

void ChatService::otoChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int toid = js["to"].get<int>();
    int id = js["id"].get<int>();

    // 查询toid是否在线
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(toid);
        if (it != _userConnMap.end())
        {
            // toid在线, 发送消息
            it->second->send(js.dump());
            return;
        }
    }

    // 向数据库查询该用户是否在线, 在线说明在不同服务器, 发布订阅
    if (_userModel.queryState(toid) == "online")
    {
        _redis.publish(toid, js.dump());
        return;
    }

    // toid不在线, 存储离线消息
    _offlineMsgModel.insert(toid, js.dump());
}

void ChatService::addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();
    int friendid = js["friendid"].get<int>();

    // 添加好友
    _friendModel.insert(userid, friendid);
}

void ChatService::createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    string name = js["groupname"];
    string desc = js["groupdesc"];
    Group group(name, desc);
    _groupModel.create(group);
}

void ChatService::addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int gid = js["groupid"].get<int>();
    int uid = js["userid"].get<int>();
    string role = js["role"];
    _groupModel.addTo(uid, gid, role);
}

void ChatService::groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int gid = js["groupid"].get<int>();
    int uid = js["userid"].get<int>();
    vector<int> uids = _groupModel.queryGroupUsersById(gid, uid);
    for (int id : uids)
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(id);
        if (it != _userConnMap.end())
        {
            it->second->send(js.dump());
        }
        // 向数据库查询该用户是否在线, 在线说明在不同服务器, 发布订阅
        else if (_userModel.queryState(id) == "online")
        {
            cout << "开始发布订阅" << endl;
            _redis.publish(id, js.dump());
        }
        else
        {
            // 存储离线消息
            _offlineMsgModel.insert(id, js.dump());
        }
    }
}

void ChatService::clientCloseException(const TcpConnectionPtr &conn)
{
    User user;
    {
        lock_guard<mutex> lock(_connMutex);
        for (auto e : _userConnMap)
        {
            if (e.second == conn)
            {
                // 从map中删除用户的连接信息, 更新数据库中的状态信息
                user.setId(e.first);
                _userConnMap.erase(e.first);
                break;
            }
        }
    }

    if (user.getId() == -1)
        return; // 没有找到对应的用户, 直接返回

    user.setState("offline");
    _userModel.updateState(user);

    // 退订
    _redis.unsubscribe(user.getId());
}

void ChatService::reset()
{
    // 将所有在线用户的状态设置为离线
    _userModel.resetState();
}
