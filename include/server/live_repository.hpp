#pragma once
#include "media_repository.hpp"

namespace chat::media {
// 直播元数据和聊天记录的持久化；不持有媒体数据或网络连接。
class LiveRepository {
public:
    explicit LiveRepository(boost::mysql::tcp_connection& connection) : connection_(connection) {}
    // 幂等创建直播与历史卡片；每位发布者最多一场未结束直播。
    boost::asio::awaitable<nlohmann::json> create(int actor, Conversation conversation,
        std::string client_id, std::string title, std::string id, std::string transport);
    // 检查会话访问权限；owner_only 另要求为发布者。
    boost::asio::awaitable<nlohmann::json> access(int actor, Conversation conversation,
        std::string id, bool owner_only = false);
    // SRS 回调登记发布者连接；结束后不能重推，旧连接的断流不能覆盖新连接。
    boost::asio::awaitable<nlohmann::json> published(std::string id, std::string client);
    boost::asio::awaitable<nlohmann::json> unpublished(std::string id, std::string client);
    // 主动结束并返回发布者连接标识，供服务层终止实际推流。
    boost::asio::awaitable<nlohmann::json> end(int actor, Conversation conversation, std::string id);
    // 读取流记录或需要核对的集合；已结束但发布连接仍在的记录也要处理。
    boost::asio::awaitable<nlohmann::json> find(std::string id);
    boost::asio::awaitable<nlohmann::json> active();
    // 仅对仍属于同一发布连接的记录校正状态，避免异步查询覆盖后来的回调。
    boost::asio::awaitable<void> reconcile(std::string id, std::string client, bool present);
private:
    boost::mysql::tcp_connection& connection_;  // 借用调用方从连接池取得的连接。
};
}
