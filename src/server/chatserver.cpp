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

void ChatServer::onMessage(const TcpConnectionPtr &conn, Buffer *buffer, Timestamp time)
{
    std::string buf = buffer->retrieveAllAsString();
    _recvBuffers[conn] += buf; // 追加到连接的缓冲区

    std::string &recvBuf = _recvBuffers[conn];
    size_t start_pos = 0;
    int brace_count = 0;
    bool in_string = false;

    for (size_t i = 0; i < recvBuf.size(); ++i)
    {
        char c = recvBuf[i];
        if (c == '"')
        {
            // 检查前面的转义符数量是否为偶数
            int backslashCount = 0;
            size_t j = i;
            while (j > 0 && recvBuf[j - 1] == '\\')
            {
                backslashCount++;
                j--;
            }
            if (backslashCount % 2 == 0)
            { // 未被转义
                in_string = !in_string;
            }
        }

        if (!in_string)
        {
            if (c == '{')
            {
                brace_count++;
            }
            else if (c == '}')
            {
                brace_count--;
                if (brace_count == 0)
                {
                    // 提取当前JSON对象
                    std::string json_str = recvBuf.substr(start_pos, i - start_pos + 1);
                    start_pos = i + 1;

                    try
                    {
                        nlohmann::json js = nlohmann::json::parse(json_str);
                        auto msgHandler = ChatService::instance()->getHandler(js["msgid"].get<int>());
                        msgHandler(conn, js, time);
                    }
                    catch (const std::exception &e)
                    {
                        std::cerr << "JSON error: " << e.what() << std::endl;
                        // conn->shutdown();
                        _recvBuffers.erase(conn);
                        return;
                    }
                }
            }
        }
    }

    // 保留未处理的数据
    if (start_pos > 0)
    {
        recvBuf = (start_pos < recvBuf.size()) ? recvBuf.substr(start_pos) : "";
    }
}