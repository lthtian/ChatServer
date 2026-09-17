// ABOUTME: Owns a chat control connection and its ordered writes.
// ABOUTME: Delivers parsed requests with bounded receive and send buffers.
#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <string>
#include <queue>
#include <functional>
#include <atomic>

#include "json.hpp"

namespace asio = boost::asio;
using asio::ip::tcp;
using json = nlohmann::json;

class Session : public std::enable_shared_from_this<Session>
{
public:
    using Ptr = std::shared_ptr<Session>;
    using MessageCallback = std::function<asio::awaitable<void>(const Ptr &, json)>;
    using CloseCallback = std::function<void(const Ptr &)>;

    Session(tcp::socket socket, MessageCallback msgCb, CloseCallback closeCb);
    ~Session();

    void start();
    void stop() { close(); }
    void send(const std::string &msg);
    std::string remote_endpoint() const;
    bool connected() const { return !closed_; }

private:
    asio::awaitable<void> read_loop();
    void do_write();
    void close();

    tcp::socket socket_;
    MessageCallback messageCallback_;
    CloseCallback closeCallback_;
    std::string recvBuf_;
    std::atomic<bool> closed_{false};
    std::queue<std::string> writeQueue_;
    std::size_t queued_bytes_ = 0;
};
