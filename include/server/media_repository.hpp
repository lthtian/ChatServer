// ABOUTME: Defines authenticated media records and image message transactions.
// ABOUTME: Uses one MySQL connection for each repository operation and transaction.
#pragma once

#include "chat/media/image.h"
#include "json.hpp"
#include <boost/asio.hpp>
#include <boost/mysql.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace chat::media {

struct Conversation {
    bool is_group;
    int target;
    std::string key(int actor) const;
};

struct UploadIntent {
    Conversation conversation;
    std::string client_msg_id;
    std::uint64_t bytes;
    std::string sha256;
    std::string kind = "image";
    std::string name;
};

class MediaError : public std::runtime_error {
public:
    explicit MediaError(const std::string& code) : std::runtime_error(code) {}
};

class MediaRepository {
public:
    explicit MediaRepository(boost::mysql::tcp_connection& connection)
        : connection_(connection) {}

    boost::asio::awaitable<nlohmann::json> begin(
        int actor, UploadIntent intent, std::string media_id, std::string provider);
    boost::asio::awaitable<nlohmann::json> owned(int actor, std::string media_id);
    boost::asio::awaitable<bool> start_processing(int actor, std::string media_id);
    boost::asio::awaitable<void> ready(int actor, std::string media_id,
                                      Thumbnail thumbnail, std::uint64_t actual_bytes);
    boost::asio::awaitable<void> ready_file(int actor, std::string media_id, std::uint64_t actual_bytes);
    boost::asio::awaitable<void> fail(int actor, std::string media_id, std::string code);
    boost::asio::awaitable<void> cancel(int actor, std::string media_id);
    boost::asio::awaitable<void> retry(int actor, std::string media_id);
    boost::asio::awaitable<nlohmann::json> garbage();
    boost::asio::awaitable<void> purged(int actor, std::string media_id);
    boost::asio::awaitable<nlohmann::json> publish(
        int actor, Conversation conversation, std::string client_msg_id, std::string media_id);
    boost::asio::awaitable<nlohmann::json> history(
        int actor, Conversation conversation, int before_id, int limit);
    boost::asio::awaitable<nlohmann::json> send_text(
        int actor, Conversation conversation, std::string client_msg_id, std::string text);
    boost::asio::awaitable<nlohmann::json> sync(
        int actor, Conversation conversation, std::string direction,
        std::int64_t cursor, std::int64_t through, int limit);
    boost::asio::awaitable<nlohmann::json> access(
        int actor, Conversation conversation, std::string media_id);

private:
    boost::asio::awaitable<void> authorize(int actor, Conversation conversation);
    boost::mysql::tcp_connection& connection_;
};

}  // namespace chat::media
