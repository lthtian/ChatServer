#include "chatservice.hpp"
#include "public.hpp"
#include <muduo/base/Logging.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
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
    _mhm.insert({InitMsg, std::bind(&ChatService::init, this, _1, _2, _3)});
    _mhm.insert({HistoryMsg, std::bind(&ChatService::history, this, _1, _2, _3)});
    _mhm.insert({RemoveFriendMsg, std::bind(&ChatService::removeFriend, this, _1, _2, _3)});
    _mhm.insert({RemoveGroupMsg, std::bind(&ChatService::removeGroup, this, _1, _2, _3)});

    _mhm.insert({NewMsg, std::bind(&ChatService::getNewMsg, this, _1, _2, _3)});
    _mhm.insert({addNewMsgCnt, std::bind(&ChatService::addNewMsg, this, _1, _2, _3)});
    _mhm.insert({removeNewMsgCnt, std::bind(&ChatService::removeNewMsg, this, _1, _2, _3)});

    _mhm.insert({imageReq, std::bind(&ChatService::getImage, this, _1, _2, _3)});

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

        // 登录只需要id和name
        json response;
        response["msgid"] = LoginMsgAck;
        response["errno"] = 0;
        response["id"] = user.getId();
        response["name"] = user.getName();

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

void ChatService::init(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int id = js["id"].get<int>();

    json response;
    response["msgid"] = InitMsgAck;

    // // 查询离线消息, 有就传入
    // vector<string> ret = _offlineMsgModel.query(id);
    // if (!ret.empty())
    // {
    //     response["offlinemsg"] = ret;
    //     // 把已经查到的信息删除
    //     _offlineMsgModel.remove(id);
    // }

    // 查询好友列表
    vector<string> friends = _friendModel.query(id);
    if (!friends.empty())
    {
        response["friends"] = friends;
    }

    // 查询群组列表
    vector<string> groups = _groupModel.queryGroups(id);
    if (!groups.empty())
    {
        response["groups"] = groups;
    }

    conn->send(response.dump());
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

string ChatService::base64_decode(const std::string &encoded)
{
    BIO *bio, *b64;
    char *buffer = new char[encoded.size()];
    memset(buffer, 0, encoded.size());

    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new_mem_buf(encoded.data(), encoded.size());
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL); // 避免换行问题

    int decoded_len = BIO_read(bio, buffer, encoded.size());
    std::string result(buffer, decoded_len);

    BIO_free_all(bio);
    delete[] buffer;
    return result;
}

// 注册任务逻辑 : 填入name, password, 插入到数据库中
void ChatService::reg(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    cout << "触发注册业务" << endl;
    string name = js["username"];
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

        // 处理头像
        if (js.contains("avatar") && js["avatar"].is_string())
        {
            string avatar_base64 = js["avatar"];

            // 解码Base64
            string avatar_binary = base64_decode(avatar_base64);

            // 存入数据库前压缩图片
            _imageModel.insert(user.getId(), avatar_binary);
        }
    }
    else
    {
        json response;
        response["msgid"] = RegMsgAck;
        response["errno"] = 1;
        response["errmsg"] = "用户已存在或输入非法!";
        conn->send(response.dump());
    }
}

void ChatService::history(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    bool flag = js["isgroup"].get<bool>();
    if (!flag)
    {
        // 接收两个id, 转化为chatkey, 向数据库查询, 查询结果返回给客户端
        int id1 = js["id1"].get<int>();
        int id2 = js["id2"].get<int>();
        vector<string> history = _messageModel.query(getChatKey(id1, id2));

        json response;
        response["isgroup"] = flag;
        response["msgid"] = HistoryMsgAck;
        response["history"] = history;
        conn->send(response.dump());
    }
    else
    {
        int groupid = js["groupid"].get<int>();
        vector<string> history = _messageModel.query(to_string(groupid));

        if (flag)
            cout << "已发送true" << endl;
        else
            cout << "已发送false" << endl;

        json response;
        response["isgroup"] = flag;
        response["msgid"] = HistoryMsgAck;
        response["history"] = history;
        conn->send(response.dump());
    }
}

void ChatService::otoChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int id = js["id"].get<int>();
    int toid = js["to"].get<int>();

    // 不管在不在线, 都将消息插入到数据库中
    _messageModel.insert(getChatKey(id, toid), false, id, js["message"].get<string>());

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

    // 走到这里说明用户不在线, 未读消息+1
    string key = to_string(toid) + "-" + to_string(id);
    _newMsgModel.addNewMsgByKey(key);
}

string ChatService::getChatKey(int id1, int id2)
{
    if (id1 < id2)
        return to_string(id1) + "#" + to_string(id2);
    else
        return to_string(id2) + "#" + to_string(id1);
}

void ChatService::addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["id"].get<int>();
    string friendname = js["friendname"];

    json response;
    response["msgid"] = AddFriendMsgAck;
    response["friendname"] = friendname;
    response["friendid"] = _userModel.query(friendname).getId();

    // 添加好友
    int ret = _friendModel.insert(userid, friendname);
    if (ret == 0)
    {

        response["errno"] = 0;
        response["errmsg"] = "添加好友成功";
        conn->send(response.dump());
    }
    else if (ret == 1)
    {
        response["errno"] = 1;
        response["errmsg"] = "该用户不存在";
        conn->send(response.dump());
    }
    else
    {
        response["errno"] = 2;
        response["errmsg"] = "该用户已经是你的好友";
        conn->send(response.dump());
    }
}

void ChatService::createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    string name = js["groupname"];
    int creatorid = js["userid"].get<int>();
    Group group(name);
    int id = _groupModel.create(group, creatorid);

    json response;
    response["msgid"] = CreateGroupMsgAck;
    if (id != -1)
    {
        response["errno"] = 0;
        response["errmsg"] = "群组创建成功";
        response["groupid"] = id;
        response["groupname"] = name;
        conn->send(response.dump());
    }
    else
    {
        response["errno"] = 1;
        response["errmsg"] = "群组创建失败";
        conn->send(response.dump());
    }
}

void ChatService::addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    string gname = js["groupname"];
    int uid = js["userid"].get<int>();
    string role = js["role"];
    int gid = _groupModel.queryGroupidByName(gname);
    bool flag;
    if (gid == -1)
        flag = false;
    else
        flag = _groupModel.addTo(uid, gid, role);

    json response;
    response["msgid"] = AddGroupMsgAck;

    if (flag)
    {
        response["errno"] = 0;
        response["errmsg"] = "群组添加成功";
        response["groupname"] = gname;
        response["groupid"] = gid;
        conn->send(response.dump());
    }
    else
    {
        response["errno"] = 1;
        response["errmsg"] = "群组添加失败";
        conn->send(response.dump());
    }
}

void ChatService::groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int gid = js["groupid"].get<int>();
    int uid = js["userid"].get<int>();

    // 将要存储的消息处理一下
    string msg = js["message"].get<string>();

    // 不管在不在线, 都将消息插入到数据库中
    _messageModel.insert(to_string(gid), true, uid, msg);

    vector<int> uids = _groupModel.queryGroupUsersById(gid, uid);
    for (int id : uids)
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(id);
        if (it != _userConnMap.end())
        {
            it->second->send(js.dump());
            continue;
        }
        // 向数据库查询该用户是否在线, 在线说明在不同服务器, 发布订阅
        if (_userModel.queryState(id) == "online")
        {
            _redis.publish(id, js.dump());
            continue;
        }
        // 走到这里说明用户不在线, 未读消息+1
        string key = to_string(id) + "-" + to_string(gid) + "-group";
        _newMsgModel.addNewMsgByKey(key);
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

void ChatService::removeFriend(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    cout << "触发removeFriend" << endl;
    int id = js["userid"].get<int>();
    int fid = js["friendid"].get<int>();

    _friendModel.remove(id, fid);

    // 如果对方在线, 发出请求更新对方客户端

    // 把以往的聊天记录删掉
    _messageModel.remove(getChatKey(id, fid));
}

void ChatService::removeGroup(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    // 先判断这个用户是否是群组的creator, 是的话移除群组, 否则移除用户
    int id = js["userid"].get<int>();
    int gid = js["groupid"].get<int>();

    bool op = _groupModel.queryRoleById(gid, id);

    if (op) // 是creator
    {
        _groupModel.removeGroupById(gid);
    }
    else // 是普通用户只要移除用户即可
    {
        _groupModel.removeUserFromGroup(id, gid);
    }

    // 如果是移除群, 把以往的聊天记录删掉
    if (op)
        _messageModel.remove(to_string(gid));
}

void ChatService::getNewMsg(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    cout << "触发getNewMsg" << endl;
    int userid = js["userid"].get<int>();
    int sender = js["sender"].get<int>();
    bool isgroup = js["isgroup"].get<bool>();
    string name = js["name"];
    cout << "已取出信息" << endl;
    // 合成key
    string key;
    if (isgroup)
        key = to_string(userid) + "-" + to_string(sender) + "-group";
    else
        key = to_string(userid) + "-" + to_string(sender);

    // 查询到key对应的cnt
    int cnt = _newMsgModel.getNewMsgCntByKey(key);

    cout << "开始将信息返回前端" << endl;
    // 连同名字一块返回前端
    json response;
    response["msgid"] = NewMsgAck;
    response["cnt"] = cnt;
    response["name"] = name;
    conn->send(response.dump());
}

void ChatService::addNewMsg(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["userid"].get<int>();
    int sender = js["sender"].get<int>();
    bool isgroup = js["isgroup"].get<bool>();
    // 合成key
    string key;
    if (isgroup)
        key = to_string(userid) + "-" + to_string(sender) + "-group";
    else
        key = to_string(userid) + "-" + to_string(sender);

    _newMsgModel.addNewMsgByKey(key);
}

void ChatService::removeNewMsg(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["userid"].get<int>();
    int sender = js["sender"].get<int>();
    bool isgroup = js["isgroup"].get<bool>();
    // 合成key
    string key;
    if (isgroup)
        key = to_string(userid) + "-" + to_string(sender) + "-group";
    else
        key = to_string(userid) + "-" + to_string(sender);

    _newMsgModel.removeNewMsgByKey(key);
}

void ChatService::getImage(const TcpConnectionPtr &conn, json &js, Timestamp time)
{
    int userid = js["userid"].get<int>();
    cout << "已发送" << userid << "的头像" << endl;
    string base64_image = _imageModel.query(userid);
    cout << "触发getImage" << endl;
    json response;
    response["msgid"] = imageReqAck;
    response["userid"] = userid;
    response["username"] = js["username"].get<string>();
    if (base64_image.empty())
    {
        response["isSuccess"] = "false";
        conn->send(response.dump());
        return;
    }

    response["isSuccess"] = "true";
    response["image_data"] = base64_image;
    conn->send(response.dump());
}