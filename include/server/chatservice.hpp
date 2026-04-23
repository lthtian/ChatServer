#pragma once

// 业务模块(单例模式)
#include "session.hpp"
#include <boost/asio.hpp>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <chrono>
using namespace std;

#include "json.hpp"
using json = nlohmann::json;

#include "usermodel.hpp"
#include "offlinemsgmodel.hpp"
#include "messagemodel.hpp"
#include "friendmodel.hpp"
#include "groupmodel.hpp"
#include "newmsgmodel.hpp"
#include "imagemodel.hpp"
#include "redis.hpp"

namespace asio = boost::asio;

// Timestamp类型替代muduo::Timestamp
using Timestamp = std::chrono::steady_clock::time_point;

// 全局数据库线程池（替代 WorkerThreadPool，用于包装阻塞 DB 操作）
inline asio::thread_pool& db_pool() {
    static asio::thread_pool pool(4);
    return pool;
}

// 将阻塞函数投递到 db_pool 线程池执行并 co_await 结果
template<typename Func>
auto run_on_db(Func func) -> asio::awaitable<decltype(func())> {
    using R = decltype(func());
    co_return co_await asio::co_spawn(
        db_pool().get_executor(),
        [f = std::move(func)]() mutable -> asio::awaitable<R> {
            if constexpr (std::is_void_v<R>) {
                f();
                co_return;
            } else {
                co_return f();
            }
        },
        asio::use_awaitable
    );
}

// 表示处理消息的事件回调方法类型（协程化）
// 只要是给网络层的回调函数就必须符合这个函数签名
using MsgHandler = std::function<asio::awaitable<void>(const Session::Ptr &session, json js, Timestamp time)>;

class ChatService
{
public:
    // 单例接口
    static ChatService *instance();
    // 获取消息类型对应的处理函数
    MsgHandler getHandler(int msgid);
    // 服务器异常退出重置
    void reset();
    // 处理客户端异常退出
    void clientCloseException(const Session::Ptr &session);
    // 处理消息队列中有订阅的情况
    void handleRedisSubscribeMessage(int, string);

    // 以下编写各种业务处理协程函数
    // 处理登录业务
    asio::awaitable<void> login(const Session::Ptr &session, json js, Timestamp time);
    // 处理注销业务
    asio::awaitable<void> loginout(const Session::Ptr &session, json js, Timestamp time);
    // 处理注册业务
    asio::awaitable<void> reg(const Session::Ptr &session, json js, Timestamp time);
    // 处理一对一聊天业务
    asio::awaitable<void> otoChat(const Session::Ptr &session, json js, Timestamp time);
    // 处理好友注册服务
    asio::awaitable<void> addFriend(const Session::Ptr &session, json js, Timestamp time);
    // 处理创建群组的任务
    asio::awaitable<void> createGroup(const Session::Ptr &session, json js, Timestamp time);
    // 处理加入群组的任务
    asio::awaitable<void> addGroup(const Session::Ptr &session, json js, Timestamp time);
    // 处理群组聊天业务
    asio::awaitable<void> groupChat(const Session::Ptr &session, json js, Timestamp time);
    // 处理前端界面初始化问题
    asio::awaitable<void> init(const Session::Ptr &session, json js, Timestamp time);
    // 处理获取历史信息
    asio::awaitable<void> history(const Session::Ptr &session, json js, Timestamp time);
    // 处理移除好友服务
    asio::awaitable<void> removeFriend(const Session::Ptr &session, json js, Timestamp time);
    // 处理移除群组服务
    asio::awaitable<void> removeGroup(const Session::Ptr &session, json js, Timestamp time);
    // 处理未读消息服务
    asio::awaitable<void> getNewMsg(const Session::Ptr &session, json js, Timestamp time);
    // 增加未读消息数
    asio::awaitable<void> addNewMsg(const Session::Ptr &session, json js, Timestamp time);
    // 移除未读消息数
    asio::awaitable<void> removeNewMsg(const Session::Ptr &session, json js, Timestamp time);
    // 返回头像图片
    asio::awaitable<void> getImage(const Session::Ptr &session, json js, Timestamp time);

private:
    // 在此绑定消息类型对应处理函数
    ChatService();
    // 利用两个id生成chatkey
    string getChatKey(int id1, int id2);
    string base64_decode(const std::string &encoded);

    // 存储所有消息类型对应的处理函数
    unordered_map<int, MsgHandler> _mhm; // <消息类型, 消息处理函数>

    // 存储用户连接信息
    unordered_map<int, Session::Ptr> _userConnMap; // <用户id, 连接对象>

    // 定义互斥锁, 保证_userConnMap的线程安全
    mutex _connMutex;

    // 数据操作类对象
    UserModel _userModel;
    // 离线消息表操作对象
    OfflineMsgModel _offlineMsgModel;
    // 消息表操作对象
    MessageModel _messageModel;
    // 好友表操作对象
    FriendModel _friendModel;
    // 群组表操作对象
    GroupModel _groupModel;
    // 未读消息操作对象
    NewMsgModel _newMsgModel;
    // 图片处理操作对象
    ImageModel _imageModel;
    // redis对象
    Redis _redis;
};
