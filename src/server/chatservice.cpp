#include "chatservice.hpp"
#include "public.hpp"
#include "connectionpool.h"
#include "log.hpp"
#include <openssl/bio.h>
#include <openssl/evp.h>
using namespace std;

ChatService *ChatService::instance()
{
    static ChatService service;
    return &service;
}

ChatService::ChatService()
{
    // 初始化数据库连接池
    DBConfig dbConfig;
    dbConfig.server = "127.0.0.1";
    dbConfig.user = "lth";
    dbConfig.password = "040915lLth!";
    dbConfig.dbname = "chat";
    dbConfig.port = 3306;

    LOG_INFO << "Initializing MySQL connection pool: server=" << dbConfig.server
             << ", user=" << dbConfig.user
             << ", dbname=" << dbConfig.dbname;

    int connectionCount = 10;
    ConnectionPool::instance()->init(dbConfig, connectionCount);

    int availableCount = ConnectionPool::instance()->getAvailableCount();
    LOG_INFO << "Database connection pool initialized. Available connections: " << availableCount;

    // 为每个消息类型注册对应的协程 handler
    _mhm.emplace(LoginMsg, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->login(s, std::move(j), t);
    });
    _mhm.emplace(RegMsg, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->reg(s, std::move(j), t);
    });
    _mhm.emplace(OTOMsg, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->otoChat(s, std::move(j), t);
    });
    _mhm.emplace(AddFriendMsg, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->addFriend(s, std::move(j), t);
    });
    _mhm.emplace(CreateGroupMsg, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->createGroup(s, std::move(j), t);
    });
    _mhm.emplace(AddGroupMsg, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->addGroup(s, std::move(j), t);
    });
    _mhm.emplace(GroupChatMsg, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->groupChat(s, std::move(j), t);
    });
    _mhm.emplace(loginOutMsg, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->loginout(s, std::move(j), t);
    });
    _mhm.emplace(InitMsg, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->init(s, std::move(j), t);
    });
    _mhm.emplace(HistoryMsg, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->history(s, std::move(j), t);
    });
    _mhm.emplace(RemoveFriendMsg, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->removeFriend(s, std::move(j), t);
    });
    _mhm.emplace(RemoveGroupMsg, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->removeGroup(s, std::move(j), t);
    });
    _mhm.emplace(NewMsg, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->getNewMsg(s, std::move(j), t);
    });
    _mhm.emplace(addNewMsgCnt, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->addNewMsg(s, std::move(j), t);
    });
    _mhm.emplace(removeNewMsgCnt, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->removeNewMsg(s, std::move(j), t);
    });
    _mhm.emplace(imageReq, [this](const Session::Ptr &s, json j, Timestamp t) -> asio::awaitable<void> {
        co_return co_await this->getImage(s, std::move(j), t);
    });

    // 注册redis服务并且绑定回调函数
    if (_redis.connect())
    {
        _redis.init_notify_handler(std::bind(&ChatService::handleRedisSubscribeMessage, this, std::placeholders::_1, std::placeholders::_2));
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
        LOG_INFO << "[REDIS] SUBSCRIBE recv for " << id << " (delivered)";
        return;
    }

    _offlineMsgModel.insert(id, msg);
    LOG_INFO << "[REDIS] SUBSCRIBE recv for " << id << " (offline)";
}

// 获取消息类型对应的处理器
MsgHandler ChatService::getHandler(int msgid)
{
    auto it = _mhm.find(msgid);
    if (it == _mhm.end())
    {
        return [=](const Session::Ptr &, json, Timestamp) -> asio::awaitable<void>
        {
            LOG_ERROR << "msgid:" << msgid << " can not find handler!";
            co_return;
        };
    }
    else
        return _mhm[msgid];
}

asio::awaitable<void> ChatService::login(const Session::Ptr &session, json js, Timestamp time)
{
    string name = js["username"];
    string password = js["password"];
    User user = co_await run_on_db([this, name]() { return _userModel.query(name); });

    if (user.getName() == name && user.getPwd() == password)
    {
        if (user.getState() == "online")
        {
            json response;
            response["msgid"] = LoginMsgAck;
            response["errmsg"] = "该用户已经登录, 不能重复登录";
            response["id"] = user.getId();
            response["errno"] = 1;
            session->send(response.dump());
            LOG_WARN << "[LOGIN] Failed: user " << user.getId() << " already online";
            co_return;
        }

        user.setState("online");
        co_await run_on_db([this, user]() mutable { _userModel.updateState(user); });

        {
            lock_guard<mutex> lock(_connMutex);
            _userConnMap.insert({user.getId(), session});
        }

        _redis.subscribe(user.getId());

        json response;
        response["msgid"] = LoginMsgAck;
        response["errno"] = 0;
        response["id"] = user.getId();
        response["name"] = user.getName();

        session->send(response.dump());
        LOG_INFO << "[LOGIN] userId=" << user.getId() << " name=" << name;
    }
    else
    {
        json response;
        response["msgid"] = LoginMsgAck;
        response["errno"] = 2;
        response["errmsg"] = "用户不存在或密码错误";
        session->send(response.dump());
        LOG_WARN << "[LOGIN] Failed: invalid credentials for " << name;
    }
}

asio::awaitable<void> ChatService::init(const Session::Ptr &session, json js, Timestamp time)
{
    int id = js["id"].get<int>();

    json response;
    response["msgid"] = InitMsgAck;

    vector<string> friends = co_await run_on_db([this, id]() { return _friendModel.query(id); });
    if (!friends.empty())
    {
        response["friends"] = friends;
    }

    vector<string> groups = co_await run_on_db([this, id]() { return _groupModel.queryGroups(id); });
    if (!groups.empty())
    {
        response["groups"] = groups;
    }

    session->send(response.dump());
}

asio::awaitable<void> ChatService::loginout(const Session::Ptr &session, json js, Timestamp time)
{
    int userid = js["id"].get<int>();
    User user(userid, "", "", "offline");
    co_await run_on_db([this, user]() mutable { _userModel.updateState(user); });

    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(userid);
        if (it != _userConnMap.end())
            _userConnMap.erase(userid);
    }

    _redis.unsubscribe(userid);
    LOG_INFO << "[LOGOUT] userId=" << userid;
}

string ChatService::base64_decode(const std::string &encoded)
{
    BIO *bio, *b64;
    char *buffer = new char[encoded.size()];
    memset(buffer, 0, encoded.size());

    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new_mem_buf(encoded.data(), encoded.size());
    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);

    int decoded_len = BIO_read(bio, buffer, encoded.size());
    std::string result(buffer, decoded_len);

    BIO_free_all(bio);
    delete[] buffer;
    return result;
}

asio::awaitable<void> ChatService::reg(const Session::Ptr &session, json js, Timestamp time)
{
    string name = js["username"];
    string password = js["password"];

    // insert 会修改 user 的 id（自增ID），需要返回修改后的 user
    auto [ret, insertedUser] = co_await run_on_db([this, name, password]() mutable -> pair<bool, User> {
        User user;
        user.setName(name);
        user.setPwd(password);
        bool r = _userModel.insert(user);
        return {r, user};
    });

    if (ret)
    {
        LOG_INFO << "[REGISTER] Success: " << name << " (userId:" << insertedUser.getId() << ")";
        json response;
        response["msgid"] = RegMsgAck;
        response["errno"] = 0;
        response["id"] = insertedUser.getId();
        session->send(response.dump());

        if (js.contains("avatar") && js["avatar"].is_string())
        {
            string avatar_base64 = js["avatar"];
            string avatar_binary = base64_decode(avatar_base64);
            int uid = insertedUser.getId();
            co_await run_on_db([this, uid, avatar_binary]() { _imageModel.insert(uid, avatar_binary); });
        }
    }
    else
    {
        LOG_WARN << "[REGISTER] Failed: " << name << " already exists";
        json response;
        response["msgid"] = RegMsgAck;
        response["errno"] = 1;
        response["errmsg"] = "用户已存在或输入非法!";
        session->send(response.dump());
    }
}

asio::awaitable<void> ChatService::history(const Session::Ptr &session, json js, Timestamp time)
{
    bool flag = js["isgroup"].get<bool>();
    if (!flag)
    {
        int id1 = js["id1"].get<int>();
        int id2 = js["id2"].get<int>();
        vector<string> hist = co_await run_on_db([this, key = getChatKey(id1, id2)]() { return _messageModel.query(key); });

        json response;
        response["isgroup"] = flag;
        response["msgid"] = HistoryMsgAck;
        response["history"] = hist;
        session->send(response.dump());
    }
    else
    {
        int groupid = js["groupid"].get<int>();
        vector<string> hist = co_await run_on_db([this, groupid]() { return _messageModel.query(to_string(groupid)); });

        json response;
        response["isgroup"] = flag;
        response["msgid"] = HistoryMsgAck;
        response["history"] = hist;
        session->send(response.dump());
    }
}

asio::awaitable<void> ChatService::otoChat(const Session::Ptr &session, json js, Timestamp time)
{
    int id = js["id"].get<int>();
    int toid = js["to"].get<int>();
    string msg = js["message"].get<string>();
    string chatkey = getChatKey(id, toid);

    co_await run_on_db([this, chatkey, id, msg]() { _messageModel.insert(chatkey, false, id, msg); });

    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(toid);
        if (it != _userConnMap.end())
        {
            it->second->send(js.dump());
            LOG_INFO << "[CHAT] " << id << " -> " << toid << " (oto, local)";
            co_return;
        }
    }

    string state = co_await run_on_db([this, toid]() { return _userModel.queryState(toid); });
    if (state == "online")
    {
        _redis.publish(toid, js.dump());
        LOG_INFO << "[CHAT] " << id << " -> " << toid << " (oto, redis)";
        co_return;
    }

    string key = to_string(toid) + "-" + to_string(id);
    co_await run_on_db([this, key]() { _newMsgModel.addNewMsgByKey(key); });
    LOG_INFO << "[CHAT] " << id << " -> " << toid << " (oto, offline)";
}

string ChatService::getChatKey(int id1, int id2)
{
    if (id1 < id2)
        return to_string(id1) + "#" + to_string(id2);
    else
        return to_string(id2) + "#" + to_string(id1);
}

asio::awaitable<void> ChatService::addFriend(const Session::Ptr &session, json js, Timestamp time)
{
    int userid = js["id"].get<int>();
    string friendname = js["friendname"];

    json response;
    response["msgid"] = AddFriendMsgAck;
    response["friendname"] = friendname;

    User friendUser = co_await run_on_db([this, friendname]() { return _userModel.query(friendname); });
    response["friendid"] = friendUser.getId();

    int ret = co_await run_on_db([this, userid, friendname]() { return _friendModel.insert(userid, friendname); });
    if (ret == 0)
    {
        LOG_INFO << "[FRIEND] " << userid << " added " << friendname;
        response["errno"] = 0;
        response["errmsg"] = "添加好友成功";
        session->send(response.dump());
    }
    else if (ret == 1)
    {
        LOG_WARN << "[FRIEND] Failed: " << friendname << " not found";
        response["errno"] = 1;
        response["errmsg"] = "该用户不存在";
        session->send(response.dump());
    }
    else
    {
        LOG_WARN << "[FRIEND] Failed: " << friendname << " already friend";
        response["errno"] = 2;
        response["errmsg"] = "该用户已经是你的好友";
        session->send(response.dump());
    }
}

asio::awaitable<void> ChatService::createGroup(const Session::Ptr &session, json js, Timestamp time)
{
    string name = js["groupname"];
    int creatorid = js["userid"].get<int>();
    Group group(name);
    int id = co_await run_on_db([this, group, creatorid]() mutable { return _groupModel.create(group, creatorid); });

    json response;
    response["msgid"] = CreateGroupMsgAck;
    if (id != -1)
    {
        LOG_INFO << "[GROUP] Created: " << name << " (id:" << id << ") by " << creatorid;
        response["errno"] = 0;
        response["errmsg"] = "群组创建成功";
        response["groupid"] = id;
        response["groupname"] = name;
        session->send(response.dump());
    }
    else
    {
        LOG_ERROR << "[GROUP] Failed to create: " << name;
        response["errno"] = 1;
        response["errmsg"] = "群组创建失败";
        session->send(response.dump());
    }
}

asio::awaitable<void> ChatService::addGroup(const Session::Ptr &session, json js, Timestamp time)
{
    string gname = js["groupname"];
    int uid = js["userid"].get<int>();
    string role = js["role"];
    int gid = co_await run_on_db([this, gname]() { return _groupModel.queryGroupidByName(gname); });
    bool flag;
    if (gid == -1)
        flag = false;
    else
        flag = co_await run_on_db([this, uid, gid, role]() { return _groupModel.addTo(uid, gid, role); });

    json response;
    response["msgid"] = AddGroupMsgAck;

    if (flag)
    {
        LOG_INFO << "[GROUP] " << uid << " joined " << gname << " (id:" << gid << ")";
        response["errno"] = 0;
        response["errmsg"] = "群组添加成功";
        response["groupname"] = gname;
        response["groupid"] = gid;
        session->send(response.dump());
    }
    else
    {
        LOG_WARN << "[GROUP] Failed: " << uid << " join " << gname;
        response["errno"] = 1;
        response["errmsg"] = "群组添加失败";
        session->send(response.dump());
    }
}

asio::awaitable<void> ChatService::groupChat(const Session::Ptr &session, json js, Timestamp time)
{
    int gid = js["groupid"].get<int>();
    int uid = js["userid"].get<int>();
    string msg = js["message"].get<string>();

    co_await run_on_db([this, gid, uid, msg]() { _messageModel.insert(to_string(gid), true, uid, msg); });

    vector<int> uids = co_await run_on_db([this, gid, uid]() { return _groupModel.queryGroupUsersById(gid, uid); });
    int localCnt = 0, redisCnt = 0, offlineCnt = 0;

    for (int id : uids)
    {
        {
            lock_guard<mutex> lock(_connMutex);
            auto it = _userConnMap.find(id);
            if (it != _userConnMap.end())
            {
                it->second->send(js.dump());
                localCnt++;
                continue;
            }
        }
        string state = co_await run_on_db([this, id]() { return _userModel.queryState(id); });
        if (state == "online")
        {
            _redis.publish(id, js.dump());
            redisCnt++;
            continue;
        }
        string key = to_string(id) + "-" + to_string(gid) + "-group";
        co_await run_on_db([this, key]() { _newMsgModel.addNewMsgByKey(key); });
        offlineCnt++;
    }
    LOG_INFO << "[GROUP] " << uid << " -> group:" << gid
             << " (local:" << localCnt << " redis:" << redisCnt << " offline:" << offlineCnt << ")";
}

void ChatService::clientCloseException(const Session::Ptr &session)
{
    User user;
    {
        lock_guard<mutex> lock(_connMutex);
        for (auto e : _userConnMap)
        {
            if (e.second == session)
            {
                user.setId(e.first);
                _userConnMap.erase(e.first);
                LOG_INFO << "[DISCONNECT] userId=" << user.getId() << " (abnormal)";
                break;
            }
        }
    }

    if (user.getId() == -1)
        return;

    user.setState("offline");
    _userModel.updateState(user);

    _redis.unsubscribe(user.getId());
}

void ChatService::reset()
{
    _userModel.resetState();
}

asio::awaitable<void> ChatService::removeFriend(const Session::Ptr &session, json js, Timestamp time)
{
    int id = js["userid"].get<int>();
    int fid = js["friendid"].get<int>();
    string chatkey = getChatKey(id, fid);

    co_await run_on_db([this, id, fid, chatkey]() {
        _friendModel.remove(id, fid);
        _messageModel.remove(chatkey);
    });
}

asio::awaitable<void> ChatService::removeGroup(const Session::Ptr &session, json js, Timestamp time)
{
    int id = js["userid"].get<int>();
    int gid = js["groupid"].get<int>();

    bool op = co_await run_on_db([this, gid, id]() { return _groupModel.queryRoleById(gid, id); });

    if (op)
    {
        co_await run_on_db([this, gid]() {
            _groupModel.removeGroupById(gid);
            _messageModel.remove(to_string(gid));
        });
    }
    else
    {
        co_await run_on_db([this, id, gid]() { _groupModel.removeUserFromGroup(id, gid); });
    }
}

asio::awaitable<void> ChatService::getNewMsg(const Session::Ptr &session, json js, Timestamp time)
{
    int userid = js["userid"].get<int>();
    int sender = js["sender"].get<int>();
    bool isgroup = js["isgroup"].get<bool>();
    string name = js["name"];

    string key;
    if (isgroup)
        key = to_string(userid) + "-" + to_string(sender) + "-group";
    else
        key = to_string(userid) + "-" + to_string(sender);

    int cnt = co_await run_on_db([this, key]() { return _newMsgModel.getNewMsgCntByKey(key); });

    json response;
    response["msgid"] = NewMsgAck;
    response["cnt"] = cnt;
    response["name"] = name;
    session->send(response.dump());
}

asio::awaitable<void> ChatService::addNewMsg(const Session::Ptr &session, json js, Timestamp time)
{
    int userid = js["userid"].get<int>();
    int sender = js["sender"].get<int>();
    bool isgroup = js["isgroup"].get<bool>();

    string key;
    if (isgroup)
        key = to_string(userid) + "-" + to_string(sender) + "-group";
    else
        key = to_string(userid) + "-" + to_string(sender);

    co_await run_on_db([this, key]() { _newMsgModel.addNewMsgByKey(key); });
}

asio::awaitable<void> ChatService::removeNewMsg(const Session::Ptr &session, json js, Timestamp time)
{
    int userid = js["userid"].get<int>();
    int sender = js["sender"].get<int>();
    bool isgroup = js["isgroup"].get<bool>();

    string key;
    if (isgroup)
        key = to_string(userid) + "-" + to_string(sender) + "-group";
    else
        key = to_string(userid) + "-" + to_string(sender);

    co_await run_on_db([this, key]() { _newMsgModel.removeNewMsgByKey(key); });
}

asio::awaitable<void> ChatService::getImage(const Session::Ptr &session, json js, Timestamp time)
{
    int userid = js["userid"].get<int>();
    string base64_image = co_await run_on_db([this, userid]() { return _imageModel.query(userid); });

    json response;
    response["msgid"] = imageReqAck;
    response["userid"] = userid;
    response["username"] = js["username"].get<string>();
    if (base64_image.empty())
    {
        response["isSuccess"] = "false";
        session->send(response.dump());
        co_return;
    }

    response["isSuccess"] = "true";
    response["image_data"] = base64_image;
    session->send(response.dump());
}
