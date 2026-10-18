#include "live_repository.hpp"
#include "database_query.hpp"
#include <algorithm>
#include <regex>

namespace chat::media {
namespace asio = boost::asio;
using json = nlohmann::json;
using db::Execute;
using db::Value;
namespace {
const std::string kSelect = "SELECT live_id,owner_id,title,state,chatkey,isgroup,publisher_id FROM LiveSession ";
json Record(boost::mysql::row_view row) {
    return {{"live_id", Value(row[0])}, {"owner_id", Value(row[1])}, {"title", Value(row[2])},
        {"state", Value(row[3])}, {"chatkey", Value(row[4])}, {"is_group", row[5].as_int64() != 0},
        {"publisher_id", Value(row[6])}};
}
}

asio::awaitable<json> LiveRepository::find(std::string id) {
    if (!std::regex_match(id, std::regex("[0-9a-f]{32}"))) throw MediaError("invalid_argument");
    const auto rows = co_await Execute(connection_, kSelect + "WHERE live_id=?", id);
    if (rows.rows().empty()) throw MediaError("not_found");
    co_return Record(rows.rows()[0]);
}

asio::awaitable<json> LiveRepository::access(int actor, Conversation conversation, std::string id, bool owner_only) {
    MediaRepository permissions(connection_);
    co_await permissions.authorize(actor, conversation);
    auto live = co_await find(id);
    if (live["chatkey"] != conversation.key(actor) || live["is_group"] != conversation.is_group ||
        (owner_only && live["owner_id"] != actor)) throw MediaError("forbidden");
    co_return live;
}

asio::awaitable<json> LiveRepository::create(int actor, Conversation conversation,
    std::string client_id, std::string title, std::string id) {
    if (!std::regex_match(client_id, std::regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")) ||
        title.empty() || title.size() > 480 || std::any_of(title.begin(), title.end(),
            [](unsigned char c) { return c < 32; })) throw MediaError("invalid_argument");
    co_return co_await db::Transaction(connection_, [&]() -> asio::awaitable<json> {
        // 锁住发布者行，使并发创建和幂等查询在同一事务中串行。
        co_await Execute(connection_, "SELECT id FROM User WHERE id=? FOR UPDATE", actor);
        MediaRepository permissions(connection_);
        co_await permissions.authorize(actor, conversation);
        auto previous = co_await Execute(connection_, kSelect + "WHERE owner_id=? AND client_msg_id=?", actor, client_id);
        const bool created = previous.rows().empty();
        if (created) {
            const auto active = co_await Execute(connection_, "SELECT live_id FROM LiveSession WHERE owner_id=? AND state<>'ended'", actor);
            if (!active.rows().empty()) throw MediaError("live_already_exists");
            co_await Execute(connection_, "INSERT INTO LiveSession(live_id,owner_id,client_msg_id,chatkey,isgroup,title) VALUES(?,?,?,?,?,?)",
                id, actor, client_id, conversation.key(actor), conversation.is_group ? 1 : 0, title);
            co_await Execute(connection_, "INSERT INTO History(chatkey,userid,isgroup,message,kind,client_msg_id,live_id) VALUES(?,?,?,?,'live',?,?)",
                conversation.key(actor), actor, conversation.is_group ? 1 : 0, title, client_id, id);
        } else {
            const auto live = Record(previous.rows()[0]);
            if (live["title"] != title || live["chatkey"] != conversation.key(actor) || live["is_group"] != conversation.is_group)
                throw MediaError("state_conflict");
            id = live["live_id"];
        }
        const auto rows = co_await Execute(connection_, "SELECT h.id,h.conversation_seq,u.name,"
            "CAST(UNIX_TIMESTAMP(h.time)*1000 AS UNSIGNED) FROM History h JOIN User u ON u.id=h.userid WHERE h.live_id=?", id);
        const auto row = rows.rows()[0];
        const auto live = co_await find(id);
        co_return json{{"created", created}, {"live", live}, {"message", {
            {"message_id", std::to_string(row[0].as_int64())}, {"sequence", std::to_string(row[1].as_int64())},
            {"sender_id", actor}, {"sender_name", Value(row[2])}, {"time", Value(row[3])},
            {"client_msg_id", client_id}, {"kind", "live"}, {"text", title}, {"live", live}}}};
    });
}

asio::awaitable<json> LiveRepository::published(std::string id, std::string client) {
    if (!std::regex_match(client, std::regex("[0-9A-Za-z_-]{1,96}"))) throw MediaError("invalid_argument");
    const auto result = co_await Execute(connection_, "UPDATE LiveSession SET state='live',publisher_id=? WHERE live_id=? AND state<>'ended'", client, id);
    if (result.affected_rows() != 1) throw MediaError("live_ended");
    co_return co_await find(id);
}

asio::awaitable<json> LiveRepository::unpublished(std::string id, std::string client) {
    co_await Execute(connection_, "UPDATE LiveSession SET state=IF(state='ended','ended','interrupted'),publisher_id='' "
        "WHERE live_id=? AND publisher_id=?", id, client);
    co_return co_await find(id);
}

asio::awaitable<json> LiveRepository::end(int actor, Conversation conversation, std::string id) {
    auto live = co_await access(actor, conversation, id, true);
    co_await Execute(connection_, "UPDATE LiveSession SET state='ended' WHERE live_id=?", id);
    live["state"] = "ended";
    co_return live;
}

asio::awaitable<json> LiveRepository::active() {
    const auto rows = co_await Execute(connection_, kSelect + "WHERE state<>'ended' OR publisher_id<>'' ORDER BY created_at LIMIT 128");
    json result = json::array();
    for (const auto row : rows.rows()) result.push_back(Record(row));
    co_return result;
}

asio::awaitable<void> LiveRepository::reconcile(std::string id, std::string client, bool present) {
    co_await Execute(connection_, "UPDATE LiveSession SET state=? WHERE live_id=? AND publisher_id=? AND state IN ('live','interrupted')",
        present ? "live" : "interrupted", id, client);
}
}
