#pragma once

// 业务模块(单例模式)
#include <muduo/net/TcpConnection.h>
#include <unordered_map>
#include <functional>
#include <mutex>
using namespace std;
using namespace muduo;
using namespace muduo::net;

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

// 表示处理消息的事件回调方法类型
// 只要是给网络层的回调函数就必须符合这个函数签名
using MsgHandler = std::function<void(const TcpConnectionPtr &conn, json &js, Timestamp time)>;

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
    void clientCloseException(const TcpConnectionPtr &conn);
    // 处理消息队列中有订阅的情况
    void handleRedisSubscribeMessage(int, string);

    // 以下编写各种业务处理回调函数
    // 处理登录业务
    void login(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理注销业务
    void loginout(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理注册业务
    void reg(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理一对一聊天业务
    void otoChat(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理好友注册服务
    void addFriend(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理创建群组的任务
    void createGroup(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理加入群组的任务
    void addGroup(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理群组聊天业务
    void groupChat(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理前端界面初始化问题
    void init(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理获取历史信息
    void history(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理移除好友服务
    void removeFriend(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理移除群组服务
    void removeGroup(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 处理未读消息服务
    void getNewMsg(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 增加未读消息数
    void addNewMsg(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 移除未读消息数
    void removeNewMsg(const TcpConnectionPtr &conn, json &js, Timestamp time);
    // 返回头像图片
    void getImage(const TcpConnectionPtr &conn, json &js, Timestamp time);

private:
    // 在此绑定消息类型对应处理函数
    ChatService();
    // 利用两个id生成chatkey
    string getChatKey(int id1, int id2);
    string base64_decode(const std::string &encoded);

    // 存储所有消息类型对应的处理函数
    unordered_map<int, MsgHandler> _mhm; // <消息类型, 消息处理函数>

    // 存储用户连接信息
    unordered_map<int, TcpConnectionPtr> _userConnMap; // <用户id, 连接对象>

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