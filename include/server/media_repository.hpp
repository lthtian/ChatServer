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
    // 单机处理器启动时恢复中断任务；连续三次中断后保留失败状态供人工重试。
    boost::asio::awaitable<void> recover_jobs();
    // 在事务中领取一条就绪原件的任务，并增加执行次数；空对象表示暂无任务。
    boost::asio::awaitable<nlohmann::json> claim_job();
    // 原件取消或清理后不允许将结果登记为 ready。
    boost::asio::awaitable<void> finish_job(std::string media_id, std::string failure);
    // 查询派生处理状态；空对象表示该文件未建立任务。
    boost::asio::awaitable<nlohmann::json> job_status(std::string media_id);
    // 仅文件所有者可以重试已失败任务，原件不需要重新上传。
    boost::asio::awaitable<void> retry_job(int actor, std::string media_id);
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
