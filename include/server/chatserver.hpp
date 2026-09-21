// ABOUTME: Accepts client control connections on the configured TCP endpoint.
// ABOUTME: Coordinates listener lifetime with the shared chat event loop.
#pragma once

// 网络模块

#include <boost/asio.hpp>
#include <unordered_set>

namespace asio = boost::asio;
using asio::ip::tcp;

#include "session.hpp"

class ChatServer
{
public:
    ChatServer(asio::io_context &ioc, const std::string &ip, uint16_t port);
    ~ChatServer();
    void start();
    void stop();

private:
    asio::awaitable<void> do_accept();

    asio::io_context &ioc_;
    tcp::acceptor acceptor_;
    std::unordered_set<Session::Ptr> sessions_;
};
