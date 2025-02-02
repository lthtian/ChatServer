#include "chatserver.hpp"
#include "chatservice.hpp"
#include "json.hpp"

#include <functional>
#include <string>
using namespace std;
using namespace placeholders;
using json = nlohmann::json;

ChatServer::ChatServer(EventLoop *loop, const InetAddress &listenAddr, const string &nameArg)
    : _server(loop, listenAddr, nameArg), _loop(loop)
{
    // 绑定连接建立/断开时的处理服务
    _server.setConnectionCallback(std::bind(&ChatServer::onConnection, this, _1));
    // 绑定发生消息读写时的处理服务
    _server.setMessageCallback(std::bind(&ChatServer::onMessage, this, _1, _2, _3));
    _server.setThreadNum(2);
}

void ChatServer::start()
{
    _server.start();
}

void ChatServer::onConnection(const TcpConnectionPtr &conn) // 连接回调函数指定一个参数
{
    if (!conn->connected())
    {
        // 这里可以检测到异常断开的情况
        // 可以调用业务层的接口，进行用户下线操作
        ChatService::instance()->clientCloseException(conn);
        conn->shutdown();
    }
}

void ChatServer::onMessage(const TcpConnectionPtr &conn, Buffer *buffer, Timestamp time) // 消息回调函数指定三个参数
{
    string buf = buffer->retrieveAllAsString();
    // 数据反序列化
    json js = json::parse(buf);
    // 主要目的在于解耦网络模块和业务模块
    // 通过js["msgid"]获取业务handler
    auto msgHandler = ChatService::instance()->getHandler(js["msgid"].get<int>());
    // 回调消息绑定好的事件处理器, 来执行相应的业务处理
    msgHandler(conn, js, time);
}