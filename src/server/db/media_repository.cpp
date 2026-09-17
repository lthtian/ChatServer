// ABOUTME: Persists media lifecycle changes and idempotent image messages.
// ABOUTME: Checks conversation membership before exposing media or history.
// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/09/16 16:56
#include "media_repository.hpp"
#include <algorithm>
#include <functional>
#include <string_view>
#include <vector>

namespace chat::media {
namespace asio = boost::asio;
namespace mysql = boost::mysql;
using json = nlohmann::json;

namespace {
using Connection = mysql::tcp_connection;

void Disconnect(Connection& connection) {
    boost::system::error_code ignored;
    connection.stream().close(ignored);
}

asio::awaitable<mysql::results> Query(Connection& connection, std::string sql,
                                     std::vector<mysql::field> parameters) {
    auto statement = co_await connection.async_prepare_statement(sql, asio::use_awaitable);
    std::vector<mysql::field_view> views;
    for (const auto& parameter : parameters) views.emplace_back(parameter);
    mysql::results result;
    std::exception_ptr failure;
    try {
        co_await connection.async_execute(statement.bind(views.begin(), views.end()),
                                          result, asio::use_awaitable);
    } catch (...) {
        failure = std::current_exception();
    }
    boost::system::error_code close_error;
    co_await connection.async_close_statement(statement,
        asio::redirect_error(asio::use_awaitable, close_error));
    if (close_error) Disconnect(connection);
    if (failure) std::rethrow_exception(failure);
    if (close_error) throw boost::system::system_error(close_error);
    co_return result;
}

template<class... Args>
asio::awaitable<mysql::results> Execute(Connection& connection, std::string_view sql,
                                       Args&&... args) {
    return Query(connection, std::string(sql),
                 std::vector<mysql::field>{mysql::field(std::forward<Args>(args))...});
}

asio::awaitable<json> Transaction(Connection& connection,
                                 std::function<asio::awaitable<json>()> body) {
    mysql::results result;
    co_await connection.async_execute("START TRANSACTION", result, asio::use_awaitable);
    std::exception_ptr failure;
    json output;
    try {
        output = co_await body();
        co_await connection.async_execute("COMMIT", result, asio::use_awaitable);
    } catch (...) {
        failure = std::current_exception();
    }
    if (failure) {
        boost::system::error_code error;
        co_await connection.async_execute("ROLLBACK", result,
            asio::redirect_error(asio::use_awaitable, error));
        if (error) Disconnect(connection);
        std::rethrow_exception(failure);
    }
    co_return output;
}

bool Hex(std::string_view value, std::size_t length) {
    return value.size() == length && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool ClientId(std::string_view value) {
    if (value.size() != 36) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') return false;
        } else if (!Hex(value.substr(index, 1), 1)) return false;
    }
    return true;
}

json Value(mysql::field_view field) {
    if (field.is_null()) return nullptr;
    if (field.is_int64()) return field.as_int64();
    if (field.is_uint64()) return field.as_uint64();
    return std::string(field.as_string());
}

const std::string kMediaSelect =
    "SELECT media_id,owner_id,client_msg_id,chatkey,isgroup,state,provider,original_key,"
    "thumbnail_key,expected_bytes,actual_bytes,sha256,mime,width,height,thumbnail_mime,"
    "thumbnail_bytes,thumbnail_width,thumbnail_height,failure_code,"
    "CAST(UNIX_TIMESTAMP(expires_at)*1000 AS UNSIGNED),expires_at<=CURRENT_TIMESTAMP(6) "
    "FROM Media ";

json Media(mysql::row_view row) {
    constexpr const char* names[] = {
        "media_id", "owner_id", "client_msg_id", "chatkey", "isgroup", "state", "provider",
        "original_key", "thumbnail_key", "expected_bytes", "actual_bytes", "sha256", "mime",
        "width", "height", "thumbnail_mime", "thumbnail_bytes", "thumbnail_width",
        "thumbnail_height", "failure_code", "expires_at", "expired"};
    json media = json::object();
    for (std::size_t index = 0; index < std::size(names); ++index) {
        media[names[index]] = Value(row[index]);
    }
    return media;
}

const std::string kMessageSelect =
    "SELECT h.id,h.userid,u.name,h.kind,h.message,"
    "CAST(UNIX_TIMESTAMP(h.time)*1000 AS UNSIGNED),h.client_msg_id,"
    "m.media_id,m.mime,m.actual_bytes,m.width,m.height,m.thumbnail_mime,"
    "m.thumbnail_bytes,m.thumbnail_width,m.thumbnail_height "
    "FROM History h JOIN User u ON u.id=h.userid LEFT JOIN Media m ON m.media_id=h.media_id ";

json Message(mysql::row_view row) {
    json message = {{"message_id", std::to_string(row[0].as_int64())},
        {"sender_id", Value(row[1])}, {"sender_name", Value(row[2])},
        {"kind", Value(row[3])}, {"text", Value(row[4])}, {"time", Value(row[5])},
        {"client_msg_id", Value(row[6])}};
    if (!row[7].is_null()) {
        message["media"] = {{"media_id", Value(row[7])}, {"mime", Value(row[8])},
            {"bytes", Value(row[9])}, {"width", Value(row[10])}, {"height", Value(row[11])},
            {"thumbnail", {{"mime", Value(row[12])}, {"bytes", Value(row[13])},
                           {"width", Value(row[14])}, {"height", Value(row[15])}}}};
    }
    return message;
}
}  // namespace

std::string Conversation::key(int actor) const {
    if (actor <= 0 || target <= 0) throw MediaError("invalid_argument");
    if (is_group) return std::to_string(target);
    return std::to_string(std::min(actor, target)) + "#" + std::to_string(std::max(actor, target));
}

asio::awaitable<void> MediaRepository::authorize(int actor, Conversation conversation) {
    conversation.key(actor);
    const auto result = conversation.is_group
        ? co_await Execute(connection_,
            "SELECT userid FROM GroupUser WHERE userid=? AND groupid=? FOR SHARE",
            actor, conversation.target)
        : co_await Execute(connection_,
            "SELECT userid FROM Friend WHERE (userid=? AND friendid=?) OR "
            "(userid=? AND friendid=?) FOR SHARE", actor, conversation.target,
            conversation.target, actor);
    if (result.rows().size() != (conversation.is_group ? 1u : 2u)) throw MediaError("forbidden");
}

asio::awaitable<json> MediaRepository::begin(int actor, UploadIntent intent,
                                           std::string media_id, std::string provider) {
    if (!ClientId(intent.client_msg_id) || !Hex(media_id, 32) || !Hex(intent.sha256, 64) ||
        !intent.bytes || intent.bytes > ImageLimits{}.max_bytes ||
        (provider != "disk" && provider != "oss")) throw MediaError("invalid_argument");
    co_await authorize(actor, intent.conversation);
    const auto key = intent.conversation.key(actor);
    co_await Execute(connection_,
        "INSERT INTO Media(media_id,owner_id,client_msg_id,chatkey,isgroup,state,provider,"
        "original_key,expected_bytes,sha256,expires_at) "
        "VALUES(?,?,?,?,?,'uploading',?,?,?,?,CURRENT_TIMESTAMP(6)+INTERVAL 24 HOUR) "
        "ON DUPLICATE KEY UPDATE media_id=media_id", media_id, actor, intent.client_msg_id,
        key, intent.conversation.is_group ? 1 : 0, provider, media_id + "/original",
        intent.bytes, intent.sha256);
    const auto found = co_await Execute(connection_, kMediaSelect +
        "WHERE owner_id=? AND client_msg_id=?", actor, intent.client_msg_id);
    if (found.rows().empty()) throw MediaError("conflict");
    auto media = Media(found.rows()[0]);
    if (media["chatkey"] != key || media["isgroup"] != (intent.conversation.is_group ? 1 : 0) ||
        media["expected_bytes"] != intent.bytes || media["sha256"] != intent.sha256) {
        throw MediaError("conflict");
    }
    co_return media;
}

asio::awaitable<json> MediaRepository::owned(int actor, std::string media_id) {
    const auto found = co_await Execute(connection_, kMediaSelect + "WHERE owner_id=? AND media_id=?",
                                        actor, media_id);
    if (found.rows().empty()) throw MediaError("not_found");
    co_return Media(found.rows()[0]);
}

asio::awaitable<bool> MediaRepository::start_processing(int actor, std::string media_id) {
    co_await owned(actor, media_id);
    const auto result = co_await Execute(connection_,
        "UPDATE Media SET state='processing',failure_code=NULL WHERE owner_id=? AND media_id=? "
        "AND state='uploading' AND expires_at>CURRENT_TIMESTAMP(6)", actor, media_id);
    co_return result.affected_rows() == 1;
}

asio::awaitable<void> MediaRepository::ready(int actor, std::string media_id,
                                           Thumbnail thumbnail, std::uint64_t actual_bytes) {
    const auto result = co_await Execute(connection_,
        "UPDATE Media SET state='ready',actual_bytes=?,mime=?,width=?,height=?,thumbnail_key=?,"
        "thumbnail_mime=?,thumbnail_bytes=?,thumbnail_width=?,thumbnail_height=? "
        "WHERE owner_id=? AND media_id=? AND state='processing' AND expected_bytes=? "
        "AND expires_at>CURRENT_TIMESTAMP(6)", actual_bytes, thumbnail.source_mime,
        thumbnail.source_width, thumbnail.source_height, media_id + "/thumbnail", thumbnail.mime,
        static_cast<std::uint64_t>(thumbnail.bytes.size()), thumbnail.width, thumbnail.height,
        actor, media_id, actual_bytes);
    if (result.affected_rows() != 1) throw MediaError("state_conflict");
}

asio::awaitable<void> MediaRepository::fail(int actor, std::string media_id, std::string code) {
    if (code.empty() || code.size() > 32) throw MediaError("invalid_argument");
    co_await Execute(connection_, "UPDATE Media SET state='failed',failure_code=? "
        "WHERE owner_id=? AND media_id=? AND state IN ('uploading','processing')", code, actor, media_id);
}

asio::awaitable<void> MediaRepository::cancel(int actor, std::string media_id) {
    co_await Transaction(connection_, [&, actor]() -> asio::awaitable<json> {
        const auto media = co_await Execute(connection_, kMediaSelect +
            "WHERE owner_id=? AND media_id=? FOR UPDATE", actor, media_id);
        if (media.rows().empty()) throw MediaError("not_found");
        const auto sent = co_await Execute(connection_,
            "SELECT id FROM History WHERE media_id=? FOR UPDATE", media_id);
        if (!sent.rows().empty()) throw MediaError("already_sent");
        co_await Execute(connection_, "UPDATE Media SET state='canceled' WHERE media_id=?", media_id);
        co_return json::object();
    });
}

asio::awaitable<void> MediaRepository::retry(int actor, std::string media_id) {
    const auto result = co_await Execute(connection_,
        "UPDATE Media SET state=IF(state='ready','ready','uploading'),failure_code=NULL,"
        "expires_at=CURRENT_TIMESTAMP(6)+INTERVAL 24 HOUR "
        "WHERE owner_id=? AND media_id=? AND state<>'canceled' "
        "AND (state IN ('failed','processing') OR expires_at<=CURRENT_TIMESTAMP(6)) "
        "AND NOT EXISTS(SELECT 1 FROM History WHERE History.media_id=Media.media_id)", actor, media_id);
    if (result.affected_rows() != 1) throw MediaError("state_conflict");
}

asio::awaitable<json> MediaRepository::garbage() {
    const auto result = co_await Execute(connection_,
        "SELECT owner_id,media_id FROM Media WHERE (state='canceled' OR expires_at<=CURRENT_TIMESTAMP(6)) "
        "AND (failure_code IS NULL OR failure_code<>'purged') "
        "AND NOT EXISTS(SELECT 1 FROM History WHERE History.media_id=Media.media_id) LIMIT 32");
    json records = json::array();
    for (const auto row : result.rows()) records.push_back({{"owner", Value(row[0])}, {"id", Value(row[1])}});
    co_return records;
}

asio::awaitable<void> MediaRepository::purged(int actor, std::string media_id) {
    co_await Execute(connection_, "UPDATE Media SET state=IF(state='canceled','canceled','failed'),"
        "failure_code='purged' WHERE owner_id=? AND media_id=?", actor, media_id);
}

asio::awaitable<json> MediaRepository::publish(int actor, Conversation conversation,
                                             std::string client_msg_id, std::string media_id) {
    if (!ClientId(client_msg_id) || !Hex(media_id, 32)) throw MediaError("invalid_argument");
    co_return co_await Transaction(connection_, [&, actor]() -> asio::awaitable<json> {
        co_await authorize(actor, conversation);
        const auto found = co_await Execute(connection_, kMediaSelect +
            "WHERE owner_id=? AND media_id=? FOR UPDATE", actor, media_id);
        if (found.rows().empty()) throw MediaError("not_found");
        const auto media = Media(found.rows()[0]);
        if (media["chatkey"] != conversation.key(actor) ||
            media["isgroup"] != (conversation.is_group ? 1 : 0) ||
            media["client_msg_id"] != client_msg_id) throw MediaError("conflict");
        const auto existing = co_await Execute(connection_, kMessageSelect +
            "WHERE h.userid=? AND h.client_msg_id=?", actor, client_msg_id);
        if (!existing.rows().empty()) {
            const auto message = Message(existing.rows()[0]);
            if (!message.contains("media") || message["media"]["media_id"] != media_id) {
                throw MediaError("conflict");
            }
            co_return json{{"created", false}, {"message", message}};
        }
        if (media["state"] != "ready") throw MediaError("not_ready");
        if (media["expired"] != 0) throw MediaError("expired");
        const auto inserted = co_await Execute(connection_,
            "INSERT INTO History(chatkey,userid,isgroup,message,kind,media_id,client_msg_id) "
            "VALUES(?,?,?,'','image',?,?)", conversation.key(actor), actor,
            conversation.is_group ? 1 : 0, media_id, client_msg_id);
        const auto sent = co_await Execute(connection_, kMessageSelect + "WHERE h.id=?",
                                           inserted.last_insert_id());
        co_return json{{"created", true}, {"message", Message(sent.rows()[0])}};
    });
}

asio::awaitable<json> MediaRepository::history(int actor, Conversation conversation,
                                             int before_id, int limit) {
    if (before_id < 0 || limit < 1 || limit > 100) throw MediaError("invalid_argument");
    co_await authorize(actor, conversation);
    const auto found = co_await Execute(connection_, kMessageSelect +
        "WHERE h.chatkey=? AND h.isgroup=? AND (?=0 OR h.id<?) ORDER BY h.id DESC LIMIT ?",
        conversation.key(actor), conversation.is_group ? 1 : 0, before_id, before_id, limit + 1);
    const auto count = std::min(found.rows().size(), static_cast<std::size_t>(limit));
    json messages = json::array();
    for (auto index = count; index > 0; --index) messages.push_back(Message(found.rows()[index - 1]));
    co_return json{{"messages", messages}, {"next_cursor", found.rows().size() > count
        ? std::to_string(found.rows()[count - 1][0].as_int64()) : ""}};
}

asio::awaitable<json> MediaRepository::access(int actor, Conversation conversation, std::string media_id) {
    co_await authorize(actor, conversation);
    const auto found = co_await Execute(connection_, kMediaSelect +
        "WHERE media_id=? AND chatkey=? AND isgroup=? AND state='ready'", media_id,
        conversation.key(actor), conversation.is_group ? 1 : 0);
    if (found.rows().empty()) throw MediaError("not_found");
    const auto media = Media(found.rows()[0]);
    const auto sent = co_await Execute(connection_, "SELECT id FROM History WHERE media_id=?", media_id);
    if (sent.rows().empty() && (media["owner_id"] != actor || media["expired"] != 0)) {
        throw MediaError("not_found");
    }
    co_return media;
}

}  // namespace chat::media
