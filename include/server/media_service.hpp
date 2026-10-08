// ABOUTME: Authorizes short-lived image transfers and media control requests.
// ABOUTME: Bounds network concurrency and runs disk and codec work off the chat loop.
#pragma once
#include "media_repository.hpp"
#include "hls_catalog.hpp"
#include "media_jobs.hpp"
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
        std::shared_ptr<const HlsCatalog> hls;  // 固定到已发布版本；所有子资源共享授权。
    };
    nlohmann::json descriptor(Ticket ticket);
    boost::asio::awaitable<void> accept();
    boost::asio::awaitable<void> collect();
    boost::asio::awaitable<void> serve(boost::asio::ip::tcp::socket socket);
    boost::asio::awaitable<void> upload(boost::beast::tcp_stream& stream,
        boost::beast::flat_buffer& buffer, boost::beast::http::request_parser<boost::beast::http::buffer_body>& parser,
        Ticket ticket, nlohmann::json media);
    // 鉴权后的完整/单区间 GET；response_started 避免发送正文后再次写错误响应。
    boost::asio::awaitable<void> download(boost::beast::tcp_stream& stream, Ticket ticket,
        nlohmann::json media, std::string path, std::string range_header, bool& response_started);
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::steady_timer collection_timer_;
    boost::asio::thread_pool workers_{1};
    DiskStore store_;
    MediaJobs jobs_;  // 独立于 HTTP 磁盘读写的持久媒体处理队列。
    std::string url_;
    std::unordered_map<std::string, Ticket> tickets_;
    std::unordered_set<std::string> active_;
    std::size_t connections_ = 0;
    std::unordered_set<boost::beast::tcp_stream*> streams_;
    bool stopping_ = false;
};
}
