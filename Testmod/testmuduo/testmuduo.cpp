#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include <iostream>
#include <functional>
using namespace std;
using namespace muduo;
using namespace muduo::net;
using namespace placeholders;

// 使用muduo网络库开发服务器
// 1.创建TcpServer/EventLoop对象
// 2.创造服务器类的构造函数, 和TcpServer参数匹配
// 3.设立连接和读写的回调函数
// 4.设置合适的线程数量

class ChatServer
{
public:
    // 传入事件循环池 / IP + port / 服务器名字
    ChatServer(EventLoop *loop, const InetAddress &listenAddr, const string &nameArg)
        : _server(loop, listenAddr, nameArg), _loop(loop)
    {
        // 给服务器注册用户连接的创建和断开回调
        // 当底层监听到有用户的连接创建/断开时, 会调用自定义函数
        _server.setConnectionCallback(bind(&ChatServer::onConnection, this, _1));
        // 设置读写事件的回调
        _server.setMessageCallback(bind(&ChatServer::onMessage, this, _1, _2, _3));
        // 设置服务器端线程数量
        _server.setThreadNum(2);
    }

    void start() // 开启事件循环
    {
        _server.start();
    }

private:
    // 专门处理用户的连接创建和断开
    void onConnection(const TcpConnectionPtr &conn)
    {
        if (conn->connected())
            cout << conn->peerAddress().toIpPort() << "->"
                 << conn->localAddress().toIpPort() << "state:online" << endl;
        else
        {
            cout << conn->peerAddress().toIpPort() << "->"
                 << conn->localAddress().toIpPort() << "state:offline" << endl;
            conn->shutdown();
        }
    }

    // 专门处理用户读写事件
    void onMessage(const TcpConnectionPtr &conn, Buffer *buffer, Timestamp time)
    {
        string buf = buffer->retrieveAllAsString();
        cout << "recv data:" << buf << "time:" << time.toString() << endl;
        conn->send(buf);
    }

    TcpServer _server;
    EventLoop *_loop;
};

int main()
{
    EventLoop loop;
    InetAddress addr("127.0.0.1", 6000);
    ChatServer server(&loop, addr, "ChatServer");

    server.start();
    loop.loop();
}