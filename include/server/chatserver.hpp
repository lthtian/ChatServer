#pragma once

// 网络模块

#include <boost/asio.hpp>

namespace asio = boost::asio;
using asio::ip::tcp;

#include "session.hpp"

class ChatServer
{
public:
    ChatServer(asio::io_context &ioc, const std::string &ip, uint16_t port);
    ~ChatServer();
    void start();

private:
    asio::awaitable<void> do_accept();

    asio::io_context &ioc_;
    tcp::acceptor acceptor_;
};
