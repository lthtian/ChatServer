#include "live_service.hpp"
#include "async_connectionpool.hpp"
#include "chatservice.hpp"
#include <openssl/rand.h>
#include <array>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <unordered_set>

namespace chat::media {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;
namespace {
std::string Env(const char* name, const std::string& fallback = {}) {
    const char* value = std::getenv(name);
    return value ? value : fallback;
}
std::string Random(unsigned count) {
    std::array<unsigned char, 32> bytes{};
    if (count > bytes.size() || RAND_bytes(bytes.data(), count) != 1) throw MediaError("unavailable");
    std::string result;
    for (unsigned i = 0; i < count; ++i) {
        result += "0123456789abcdef"[bytes[i] >> 4];
        result += "0123456789abcdef"[bytes[i] & 15];
    }
    return result;
}
Conversation ConversationFrom(const json& input) {
    return {input.at("is_group").get<bool>(), input.at("target").get<int>()};
}
}

LiveService::LiveService(asio::any_io_executor executor) : acceptor_(executor), timer_(executor) {
    url_ = Env("CHAT_LIVE_URL");
    if (url_.empty()) return;
    rtmp_url_ = Env("CHAT_LIVE_RTMP_URL");
    secret_ = Env("CHAT_LIVE_HOOK_SECRET");
    if (!std::regex_match(secret_, std::regex("[0-9a-f]{64}")) || !rtmp_url_.starts_with("rtmp://") ||
        !url_.starts_with("http://") || url_.ends_with('/') || rtmp_url_.ends_with('/'))
        throw MediaError("invalid_live_configuration");
    api_port_ = static_cast<unsigned short>(std::stoi(Env("CHAT_LIVE_API_PORT", "1985")));
    const auto port = static_cast<unsigned short>(std::stoi(Env("CHAT_LIVE_PORT", "6003")));
    const tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), port);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
}

void LiveService::start(Notify notify) {
    if (!acceptor_.is_open()) return;
    notify_ = std::move(notify);
    asio::co_spawn(acceptor_.get_executor(), accept(), asio::detached);
    asio::co_spawn(acceptor_.get_executor(), monitor(), asio::detached);
}
void LiveService::stop() {
    stopping_ = true;
    boost::system::error_code ignored;
    acceptor_.close(ignored);
    timer_.cancel(ignored);
}

json LiveService::descriptor(int actor, const Session::Ptr& session, Conversation conversation,
    const std::string& id, bool publish, const std::string& format) {
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(tickets_, [now](const auto& item) { return item.second.deadline <= now; });
    if (tickets_.size() >= 1024) throw MediaError("busy");
    const auto token = Random(32);
    tickets_.emplace(token, Ticket{actor, session, conversation, id, publish, now + std::chrono::minutes(5)});
    const auto expires = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 300000;
    if (publish) return {{"server", rtmp_url_}, {"key", id + "?token=" + token},
        {"url", rtmp_url_ + "/" + id + "?token=" + token}, {"expires_at", expires}};
    return {{"method", "GET"}, {"url", url_ + "/" + id + (format == "hls" ? ".m3u8" : ".flv")},
        {"headers", {{"Authorization", "Bearer " + token}}}, {"expires_at", expires}};
}

LiveService::Ticket LiveService::ticket(const std::string& token, bool publish) const {
    const auto found = tickets_.find(token);
    if (found == tickets_.end() || found->second.publish != publish ||
        found->second.deadline <= std::chrono::steady_clock::now()) throw MediaError("expired_ticket");
    const auto session = found->second.session.lock();
    if (!session || !session->connected() || ChatService::instance()->actor(session) != found->second.actor)
        throw MediaError("unauthorized");
    return found->second;
}

asio::awaitable<json> LiveService::request(int actor, Session::Ptr session, json input) {
    if (url_.empty()) throw MediaError("live_unavailable");
    const auto op = input.at("op").get<std::string>();
    const auto conversation = ConversationFrom(input.at("conversation"));
    json result;
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid()) throw MediaError("unavailable");
        LiveRepository repository(guard->connection());
        if (op == "live_create") {
            result = co_await repository.create(actor, conversation, input.at("client_msg_id"), input.at("title"), Random(16));
        } else {
            const auto id = input.at("live_id").get<std::string>();
            if (op == "live_end") result["live"] = co_await repository.end(actor, conversation, id);
            else result["live"] = co_await repository.access(actor, conversation, id, op == "live_publish");
        }
    }
    const auto live = result.at("live");
    const auto id = live.at("live_id").get<std::string>();
    if (op == "live_create" || op == "live_publish") {
        if (live["state"] == "ended") throw MediaError("live_ended");
        result["publish"] = descriptor(actor, session, conversation, id, true, "");
    } else if (op == "live_read") {
        if (live["state"] == "ended") throw MediaError("live_ended");
        if (live["state"] != "live") throw MediaError("live_not_ready");
        const auto format = input.value("format", "flv");
        if (format != "flv" && format != "hls") throw MediaError("invalid_argument");
        result["format"] = format;
        result["download"] = descriptor(actor, session, conversation, id, false, format);
    } else if (op == "live_end") {
        // 先持久化结束，避免断开回调又将它标为 interrupted；管理 API 仅访问固定本机地址。
        const auto client = live.value("publisher_id", "");
        if (!client.empty()) {
            try { co_await srs("DELETE", "/api/v1/clients/" + client); }
            catch (const std::exception&) { std::cerr << "[LIVE] publisher disconnect deferred\n"; }
        }
        if (notify_) co_await notify_(live);
    } else if (op != "live_status") throw MediaError("invalid_argument");
    co_return result;
}

asio::awaitable<void> LiveService::authorize(std::string authorization, std::string path) {
    if (!authorization.starts_with("Bearer ")) throw MediaError("unauthorized");
    const auto access = ticket(authorization.substr(7), false);
    // 固定资源形状，禁止跳目录、任意扩展名或其他流；查询字符串不作为磁盘路径。
    const std::regex allowed("/live/" + access.id + "(?:\\.flv|\\.m3u8|-[0-9]+\\.ts)");
    if (!std::regex_match(path, allowed)) throw MediaError("forbidden");
    auto guard = co_await AsyncConnectionGuard::create();
    if (!guard->valid()) throw MediaError("unavailable");
    LiveRepository repository(guard->connection());
    const auto live = co_await repository.access(access.actor, access.conversation, access.id);
    if (live["state"] == "ended") throw MediaError("live_ended");
}

asio::awaitable<void> LiveService::hook(json input) {
    const auto id = input.at("stream").get<std::string>();
    if (input.at("app") != "live") throw MediaError("forbidden");
    const auto action = input.at("action").get<std::string>();
    json live;
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid()) throw MediaError("unavailable");
        LiveRepository repository(guard->connection());
        if (action == "on_publish") {
            std::smatch match;
            const auto param = input.value("param", "");
            if (!std::regex_match(param, match, std::regex("\\?token=([0-9a-f]{64})"))) throw MediaError("unauthorized");
            const auto access = ticket(match[1], true);
            if (access.id != id) throw MediaError("forbidden");
            co_await repository.access(access.actor, access.conversation, id, true);
            live = co_await repository.published(id, input.at("client_id"));
        } else if (action == "on_unpublish") {
            live = co_await repository.unpublished(id, input.at("client_id"));
        } else throw MediaError("invalid_argument");
    }
    if (notify_) co_await notify_(live);
}

asio::awaitable<void> LiveService::accept() {
    while (!stopping_) {
        boost::system::error_code error;
        auto socket = co_await acceptor_.async_accept(asio::redirect_error(asio::use_awaitable, error));
        if (error) co_return;
        if (connections_ >= 32) { socket.close(); continue; }
        ++connections_;
        asio::co_spawn(acceptor_.get_executor(), serve(std::move(socket)), asio::detached);
    }
}

asio::awaitable<void> LiveService::serve(tcp::socket socket) {
    beast::tcp_stream stream(std::move(socket));
    http::status status = http::status::forbidden;
    std::string body = "1";
    try {
        beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        parser.header_limit(8192);
        parser.body_limit(16384);
        stream.expires_after(std::chrono::seconds(10));
        co_await http::async_read(stream, buffer, parser, asio::use_awaitable);
        const auto& request = parser.get();
        if (request.method() == http::verb::post && request.target() == "/srs?secret=" + secret_) {
            co_await hook(json::parse(request.body()));
            body = "0";
        } else if (request.method() == http::verb::get && request.target() == "/authorize") {
            co_await authorize(std::string(request[http::field::authorization]), std::string(request["X-Live-Path"]));
            body.clear();
        } else throw MediaError("forbidden");
        status = http::status::ok;
    } catch (const std::exception&) {
        // 内部鉴权失败不回显票据或回调正文。
    }
    http::response<http::string_body> response(status, 11);
    response.set(http::field::content_type, "application/json");
    response.keep_alive(false);
    response.body() = body;
    response.prepare_payload();
    boost::system::error_code ignored;
    stream.expires_after(std::chrono::seconds(5));
    co_await http::async_write(stream, response, asio::redirect_error(asio::use_awaitable, ignored));
    --connections_;
}

asio::awaitable<json> LiveService::srs(std::string method, std::string path) {
    beast::tcp_stream stream(timer_.get_executor());
    stream.expires_after(std::chrono::seconds(3));
    co_await stream.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), api_port_), asio::use_awaitable);
    http::request<http::empty_body> request(method == "DELETE" ? http::verb::delete_ : http::verb::get, path, 11);
    request.set(http::field::host, "127.0.0.1");
    request.keep_alive(false);
    co_await http::async_write(stream, request, asio::use_awaitable);
    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(512 * 1024);
    co_await http::async_read(stream, buffer, parser, asio::use_awaitable);
    if (parser.get().result() != http::status::ok) throw MediaError("srs_unavailable");
    co_return json::parse(parser.get().body());
}

asio::awaitable<void> LiveService::monitor() {
    while (!stopping_) {
        try {
            json records;
            {
                auto guard = co_await AsyncConnectionGuard::create();
                if (!guard->valid()) throw MediaError("unavailable");
                LiveRepository repository(guard->connection());
                records = co_await repository.active();
            }
            // 先取得数据库快照，再查询媒体服务；后续更新仍匹配原发布连接，避免覆盖重连。
            const auto streams = co_await srs("GET", "/api/v1/streams/");
            std::unordered_map<std::string, std::string> present;
            for (const auto& item : streams.at("streams"))
                if (item.value("app", "") == "live" && item.at("publish").value("active", false))
                    present.emplace(item.at("name"), item.at("publish").at("cid"));
            for (auto live : records) {
                const std::string id = live["live_id"];
                const bool active = present.contains(id);
                if (live["state"] == "ended") {
                    if (active) co_await srs("DELETE", "/api/v1/clients/" + present.at(id));
                    else {
                        auto guard = co_await AsyncConnectionGuard::create();
                        if (!guard->valid()) throw MediaError("unavailable");
                        LiveRepository repository(guard->connection());
                        co_await repository.unpublished(id, live["publisher_id"]);
                    }
                    continue;
                }
                if ((live["state"] == "live") == active || live["state"] == "waiting") continue;
                {
                    auto guard = co_await AsyncConnectionGuard::create();
                    if (!guard->valid()) throw MediaError("unavailable");
                    LiveRepository repository(guard->connection());
                    co_await repository.reconcile(id, live["publisher_id"], active);
                    live = co_await repository.find(id);
                }
                if (notify_) co_await notify_(live);
            }
        } catch (const std::exception&) {
            // SRS 重启期间继续重试；不把暂时不可达误记为主动结束。
        }
        timer_.expires_after(std::chrono::seconds(3));
        boost::system::error_code ignored;
        co_await timer_.async_wait(asio::redirect_error(asio::use_awaitable, ignored));
    }
}
}
