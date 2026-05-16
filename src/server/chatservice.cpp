#include "chatservice.hpp"
#include "public.hpp"
#include "async_connectionpool.hpp"
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
    // 数据库连接池配置（实际初始化在 main.cpp 中异步执行）
    // 此处只注册 handler

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

    // Redis 连接延迟到 init_redis() 中执行
}

// 异步初始化 Redis（在 init_pool 协程中调用，服务器启动前完成）
asio::awaitable<bool> ChatService::init_redis()
{
    bool ok = co_await _redis.connect("127.0.0.1", 6379);
    if (ok)
    {
        _redis.set_notify_handler(
            std::bind(&ChatService::handleRedisSubscribeMessage, this,
                      std::placeholders::_1, std::placeholders::_2));
        LOG_INFO << "[REDIS] Connected and notify handler registered";
    }
    else
    {
        LOG_ERROR << "[REDIS] Failed to connect";
    }
    co_return ok;
}

// 如果收到publish的订阅消息, 会调用该回调函数, 向消息中对应的用户进行推送
void ChatService::handleRedisSubscribeMessage(int id, string msg)
{
    // 单线程 io_context，无需 mutex
    auto it = _userConnMap.find(id);
    if (it != _userConnMap.end())
    {
        it->second->send(msg);
        LOG_INFO << "[REDIS] SUBSCRIBE recv for " << id << " (delivered)";
        return;
    }

    // 异步存储离线消息（fire-and-forget，通过 co_spawn 投递）
    asio::co_spawn(AsyncConnectionPool::instance()->get_executor(),
        [this, id, msg = std::move(msg)]() mutable -> asio::awaitable<void> {
            co_await _offlineMsgModel.insert(id, msg);
        },
        asio::detached);
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
    try
    {
    string name = js["username"];
    string password = js["password"];
    User user = co_await _userModel.query(name);

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
        co_await _userModel.updateState(user);

        // 单线程 io_context，无需 mutex，同步维护正反向映射
        _userConnMap.insert({user.getId(), session});
        _connUserMap[session] = user.getId();

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
    catch (const std::exception& e)
    {
        LOG_ERROR << "[LOGIN] Exception: " << e.what();
    }
}

asio::awaitable<void> ChatService::init(const Session::Ptr &session, json js, Timestamp time)
{
    try
    {
    int id = js["id"].get<int>();

    json response;
    response["msgid"] = InitMsgAck;

    vector<string> friends = co_await _friendModel.query(id);
    if (!friends.empty())
    {
        response["friends"] = friends;
    }

    vector<string> groups = co_await _groupModel.queryGroups(id);
    if (!groups.empty())
    {
        response["groups"] = groups;
    }

    LOG_INFO << "[INIT] userId=" << id
             << " friends=" << friends.size() << " groups=" << groups.size()
             << " resp_size=" << response.dump().size();

    session->send(response.dump());
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "[INIT] Exception for userId=" << js.value("id", -1) << ": " << e.what();
    }
}

asio::awaitable<void> ChatService::loginout(const Session::Ptr &session, json js, Timestamp time)
{
    int userid = js["id"].get<int>();
    User user(userid, "", "", "offline");
    co_await _userModel.updateState(user);

    // 单线程 io_context，同步移除正反向映射
    auto it = _userConnMap.find(userid);
    if (it != _userConnMap.end())
    {
        _connUserMap.erase(it->second);
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
    User user;
    user.setName(name);
    user.setPwd(password);
    bool ret = co_await _userModel.insert(user);

    if (ret)
    {
        LOG_INFO << "[REGISTER] Success: " << name << " (userId:" << user.getId() << ")";
        json response;
        response["msgid"] = RegMsgAck;
        response["errno"] = 0;
        response["id"] = user.getId();
        session->send(response.dump());

        if (js.contains("avatar") && js["avatar"].is_string())
        {
            string avatar_base64 = js["avatar"];
            string avatar_binary = base64_decode(avatar_base64);
            int uid = user.getId();
            co_await _imageModel.insert(uid, avatar_binary);
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
    try
    {
    bool flag = js["isgroup"].get<bool>();
    if (!flag)
    {
        int id1 = js["id1"].get<int>();
        int id2 = js["id2"].get<int>();
        string chatkey = getChatKey(id1, id2);
        vector<string> hist = co_await _messageModel.query(chatkey);

        json response;
        response["msgid"] = HistoryMsgAck;
        response["isgroup"] = flag;
        response["id1"] = id1;
        response["id2"] = id2;
        response["history"] = hist;
        session->send(response.dump());
        LOG_INFO << "[HISTORY] oto id1=" << id1 << " id2=" << id2
                 << " chatkey=" << chatkey << " count=" << hist.size();
    }
    else
    {
        int groupid = js["groupid"].get<int>();
        vector<string> hist = co_await _messageModel.query(to_string(groupid));

        json response;
        response["msgid"] = HistoryMsgAck;
        response["isgroup"] = flag;
        response["groupid"] = groupid;
        response["history"] = hist;
        session->send(response.dump());
        LOG_INFO << "[HISTORY] group groupid=" << groupid << " count=" << hist.size();
    }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "[HISTORY] Exception: " << e.what();
    }
}

asio::awaitable<void> ChatService::otoChat(const Session::Ptr &session, json js, Timestamp time)
{
    try
    {
    int id = js["id"].get<int>();
    int toid = js["to"].get<int>();
    string msg = js["message"].get<string>();
    string chatkey = getChatKey(id, toid);

    co_await _messageModel.insert(chatkey, false, id, msg);

    // 单线程 io_context，无需 mutex
    auto it = _userConnMap.find(toid);
    if (it != _userConnMap.end())
    {
        it->second->send(js.dump());
        LOG_INFO << "[CHAT] " << id << " -> " << toid << " (oto, local)";
        co_return;
    }

    string state = co_await _userModel.queryState(toid);
    if (state == "online")
    {
        _redis.publish(toid, js.dump());
        LOG_INFO << "[CHAT] " << id << " -> " << toid << " (oto, redis)";
        co_return;
    }

    string key = to_string(toid) + "-" + to_string(id);
    co_await _newMsgModel.addNewMsgByKey(key);
    LOG_INFO << "[CHAT] " << id << " -> " << toid << " (oto, offline)";
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "[OTO_CHAT] Exception: " << e.what();
    }
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

    User friendUser = co_await _userModel.query(friendname);
    response["friendid"] = friendUser.getId();

    int ret = co_await _friendModel.insert(userid, friendname);
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
    int id = co_await _groupModel.create(group, creatorid);

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
    int gid = co_await _groupModel.queryGroupidByName(gname);
    bool flag;
    if (gid == -1)
        flag = false;
    else
        flag = co_await _groupModel.addTo(uid, gid, role);

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
    try
    {
    int gid = js["groupid"].get<int>();
    int uid = js["userid"].get<int>();
    string msg = js["message"].get<string>();

    co_await _messageModel.insert(to_string(gid), true, uid, msg);

    vector<int> uids = co_await _groupModel.queryGroupUsersById(gid, uid);
    int localCnt = 0, redisCnt = 0, offlineCnt = 0;

    for (int id : uids)
    {
        // 单线程 io_context，无需 mutex
        auto it = _userConnMap.find(id);
        if (it != _userConnMap.end())
        {
            it->second->send(js.dump());
            localCnt++;
            continue;
        }
        string state = co_await _userModel.queryState(id);
        if (state == "online")
        {
            _redis.publish(id, js.dump());
            redisCnt++;
            continue;
        }
        string key = to_string(id) + "-" + to_string(gid) + "-group";
        co_await _newMsgModel.addNewMsgByKey(key);
        offlineCnt++;
    }
    LOG_INFO << "[GROUP] " << uid << " -> group:" << gid
             << " (local:" << localCnt << " redis:" << redisCnt << " offline:" << offlineCnt << ")";
    }
    catch (const std::exception& e)
    {
        LOG_ERROR << "[GROUP_CHAT] Exception: " << e.what();
    }
}

void ChatService::clientCloseException(const Session::Ptr &session)
{
    User user;
    // O(1) 反向查找：Session → userId
    auto cit = _connUserMap.find(session);
    if (cit != _connUserMap.end())
    {
        user.setId(cit->second);
        _userConnMap.erase(cit->second);
        _connUserMap.erase(cit);
        LOG_INFO << "[DISCONNECT] userId=" << user.getId() << " (abnormal)";
    }

    if (user.getId() == -1)
        return;

    user.setState("offline");

    // 异步更新状态（fire-and-forget，通过 co_spawn 投递）
    asio::co_spawn(AsyncConnectionPool::instance()->get_executor(),
        [this, user]() mutable -> asio::awaitable<void> {
            co_await _userModel.updateState(user);
        },
        asio::detached);

    _redis.unsubscribe(user.getId());
}

void ChatService::reset()
{
    // 异步重置状态（fire-and-forget，通过 co_spawn 投递）
    asio::co_spawn(AsyncConnectionPool::instance()->get_executor(),
        [this]() -> asio::awaitable<void> {
            co_await _userModel.resetState();
        },
        asio::detached);
}

asio::awaitable<void> ChatService::removeFriend(const Session::Ptr &session, json js, Timestamp time)
{
    int id = js["userid"].get<int>();
    int fid = js["friendid"].get<int>();
    string chatkey = getChatKey(id, fid);

    co_await _friendModel.remove(id, fid);
    co_await _messageModel.remove(chatkey);
}

asio::awaitable<void> ChatService::removeGroup(const Session::Ptr &session, json js, Timestamp time)
{
    int id = js["userid"].get<int>();
    int gid = js["groupid"].get<int>();

    bool op = co_await _groupModel.queryRoleById(gid, id);

    if (op)
    {
        co_await _groupModel.removeGroupById(gid);
        co_await _messageModel.remove(to_string(gid));
    }
    else
    {
        co_await _groupModel.removeUserFromGroup(id, gid);
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

    int cnt = co_await _newMsgModel.getNewMsgCntByKey(key);

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

    co_await _newMsgModel.addNewMsgByKey(key);
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

    co_await _newMsgModel.removeNewMsgByKey(key);
}

asio::awaitable<void> ChatService::getImage(const Session::Ptr &session, json js, Timestamp time)
{
    int userid = js["userid"].get<int>();
    string base64_image = co_await _imageModel.query(userid);

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
