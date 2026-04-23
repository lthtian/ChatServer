#include "chatserver.hpp"
#include "chatservice.hpp"
#include <iostream>
using namespace std;

ChatServer::ChatServer(asio::io_context &ioc, const std::string &ip, uint16_t port)
    : ioc_(ioc),
      acceptor_(ioc, tcp::endpoint(asio::ip::make_address(ip), port))
{
}

ChatServer::~ChatServer()
{
    boost::system::error_code ec;
    acceptor_.close(ec);
}

void ChatServer::start()
{
    asio::co_spawn(ioc_, do_accept(), asio::detached);
}

asio::awaitable<void> ChatServer::do_accept()
{
    while (true)
    {
        auto socket = co_await acceptor_.async_accept(asio::use_awaitable);

        auto session = std::make_shared<Session>(
            std::move(socket),
            // MessageCallback: 直接通过 getHandler 查找并 co_await 调用 handler
            [](const Session::Ptr &s, json js) -> asio::awaitable<void> {
                auto handler = ChatService::instance()->getHandler(js["msgid"].get<int>());
                co_await handler(s, std::move(js), std::chrono::steady_clock::now());
            },
            // CloseCallback: 连接断开时通知业务层
            [](const Session::Ptr &s) {
                ChatService::instance()->clientCloseException(s);
            }
        );
        session->start();
    }
}
