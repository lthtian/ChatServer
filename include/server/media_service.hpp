// ABOUTME: Authorizes short-lived image transfers and media control requests.
// ABOUTME: Bounds network concurrency and runs disk and codec work off the chat loop.
// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/09/17 15:12
#pragma once
#include "media_repository.hpp"
#include "session.hpp"
#include "chat/media/disk_store.h"
#include <boost/beast.hpp>
#include <unordered_map>
#include <unordered_set>

namespace chat::media {
class MediaService {
public:
    MediaService(boost::asio::any_io_executor executor, std::string root, int port, std::string url);
    void start();
    void stop();
    boost::asio::awaitable<nlohmann::json> request(int actor, Session::Ptr session, nlohmann::json request);
private:
    struct Ticket {
        int actor;
        std::weak_ptr<Session> session;
        Conversation conversation;
        std::string media_id;
        std::string variant;
        bool upload;
        std::chrono::steady_clock::time_point deadline;
    };
    nlohmann::json descriptor(Ticket ticket);
    boost::asio::awaitable<void> accept();
    boost::asio::awaitable<void> collect();
    boost::asio::awaitable<void> serve(boost::asio::ip::tcp::socket socket);
    boost::asio::awaitable<void> upload(boost::beast::tcp_stream& stream,
        boost::beast::flat_buffer& buffer, boost::beast::http::request_parser<boost::beast::http::buffer_body>& parser,
        Ticket ticket, nlohmann::json media);
    boost::asio::awaitable<void> download(boost::beast::tcp_stream& stream, Ticket ticket, nlohmann::json media);
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer collection_timer_;
    boost::asio::thread_pool workers_{1};
    DiskStore store_;
    std::string url_;
    std::unordered_map<std::string, Ticket> tickets_;
    std::unordered_set<std::string> active_;
    std::size_t connections_ = 0;
    std::unordered_set<boost::beast::tcp_stream*> streams_;
    bool stopping_ = false;
};
}
