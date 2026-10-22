#include "live_service.hpp"
#include "async_connectionpool.hpp"
#include "chatservice.hpp"
#include <openssl/rand.h>
#include <array>
#include <exception>
#include <regex>
#include <vector>

namespace chat::media {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

asio::awaitable<http::response<http::string_body>> LiveService::rtc_upstream(
    http::verb method, std::string path, std::string body) {
    beast::tcp_stream stream(timer_.get_executor());
    stream.expires_after(std::chrono::seconds(8));
    co_await stream.async_connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), api_port_), asio::use_awaitable);
    http::request<http::string_body> request(method, path, 11);
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/sdp");
    request.keep_alive(false);
    request.body() = std::move(body);
    request.prepare_payload();
    co_await http::async_write(stream, request, asio::use_awaitable);
    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.header_limit(8192);
    parser.body_limit(65536);
    co_await http::async_read(stream, buffer, parser, asio::use_awaitable);
    co_return parser.release();
}

asio::awaitable<http::response<http::string_body>> LiveService::rtc_request(
    const http::request<http::string_body>& request) {
    if (rtc_url_.empty()) throw MediaError("rtc_unavailable");
    const std::string authorization(request[http::field::authorization]);
    if (!authorization.starts_with("Bearer ")) throw MediaError("unauthorized");
    const auto token = authorization.substr(7);
    const std::string path(request.target());
    std::smatch match;
    http::response<http::string_body> response(http::status::no_content, 11);
    response.set(http::field::cache_control, "private, no-store");
    if (request.method() == http::verb::delete_ &&
        std::regex_match(path, match, std::regex("/rtc/session/([0-9a-f]{32})"))) {
        const auto handle = match[1].str();
        const auto found = rtc_connections_.find(handle);
        if (found == rtc_connections_.end()) co_return response; // 允许重复关闭已释放的资源。
        const auto connection = found->second;
        if (connection.token != token) throw MediaError("forbidden");
        // 建连期限只限制创建；关闭属于已有会话，不要求再申请一张短期票据。
        const auto upstream = co_await rtc_upstream(http::verb::delete_, connection.resource);
        if (upstream.result_int() / 100 != 2) throw MediaError("srs_unavailable");
        rtc_connections_.erase(handle);
        co_return response;
    }
    if (request.method() != http::verb::post ||
        !std::regex_match(path, match, std::regex("/rtc/(publish|play)/([0-9a-f]{32})")) ||
        request[http::field::content_type] != "application/sdp" || !request.body().starts_with("v=0"))
        throw MediaError("invalid_argument");
    const bool publish = match[1] == "publish";
    const auto access = ticket(token, publish);
    if (!access.rtc || access.id != match[2].str()) throw MediaError("forbidden");
    if (rtc_connections_.size() + rtc_opening_.size() >= 128 || !rtc_opening_.insert(token).second)
        throw MediaError("busy");
    // SRS 的鉴权回调会在下面 co_await 期间进入同一业务服务，必须先登记 pending。
    struct Pending {
        LiveService* service;
        std::string token;
        ~Pending() { service->rtc_opening_.erase(token); service->rtc_clients_.erase(token); }
    } pending{this, token};
    {
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid()) throw MediaError("unavailable");
        LiveRepository repository(guard->connection());
        const auto live = co_await repository.access(access.actor, access.conversation, access.id, publish);
        if (live["transport"] != "webrtc" || live["state"] == "ended" || (!publish && live["state"] != "live"))
            throw MediaError("live_not_ready");
    }
    const auto endpoint = publish ? "/rtc/v1/whip/" : "/rtc/v1/whep/";
    auto upstream = co_await rtc_upstream(http::verb::post,
        std::string(endpoint) + "?app=camera&stream=" + access.id + "&token=" + token, request.body());
    if (upstream.result() != http::status::created || !upstream.body().starts_with("v=0"))
        throw MediaError("rtc_negotiation_failed");
    const std::string resource(upstream[http::field::location]);
    if (!resource.starts_with("/rtc/v1/whip/?action=delete&token=") ||
        resource.find("&app=camera&stream=" + access.id + "&session=") == std::string::npos)
        throw MediaError("invalid_rtc_resource");
    // 协商期间登录可能退出、权限可能撤回；确认后才能把 Answer 交给客户端。
    std::exception_ptr failure;
    try {
        ticket(token, publish);
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid()) throw MediaError("unavailable");
        LiveRepository repository(guard->connection());
        const auto live = co_await repository.access(access.actor, access.conversation, access.id, publish);
        if (live["state"] != "live") throw MediaError("live_not_ready");
    } catch (...) { failure = std::current_exception(); }
    if (failure) {
        co_await rtc_upstream(http::verb::delete_, resource);
        std::rethrow_exception(failure);
    }
    const auto client = rtc_clients_.find(token);
    if (client == rtc_clients_.end()) {
        co_await rtc_upstream(http::verb::delete_, resource);
        throw MediaError("rtc_hook_missing");
    }
    std::array<unsigned char, 16> random{};
    if (RAND_bytes(random.data(), random.size()) != 1) {
        co_await rtc_upstream(http::verb::delete_, resource);
        throw MediaError("unavailable");
    }
    std::string handle;
    for (const auto value : random) {
        handle += "0123456789abcdef"[value >> 4];
        handle += "0123456789abcdef"[value & 15];
    }
    rtc_connections_.emplace(handle, RtcConnection{access, token, resource, client->second,
        std::chrono::steady_clock::now()});
    tickets_.erase(token); // 一次建连完成后，票据仅可用于删除其对应会话。
    response.result(http::status::created);
    response.set(http::field::content_type, "application/sdp");
    response.set(http::field::location, "/rtc/session/" + handle);
    response.body() = std::move(upstream.body());
    co_return response;
}

asio::awaitable<void> LiveService::prune_rtc() {
    if (rtc_url_.empty()) co_return;
    const auto clients = co_await srs("GET", "/api/v1/clients/?start=0&count=1000");
    std::unordered_set<std::string> present;
    for (const auto& client : clients.at("clients")) present.insert(client.at("id"));
    for (const auto& client : clients.at("clients")) {
        const std::string id = client.at("id"), url = client.value("url", "");
        // 默认 vhost 的 URL 以 /camera 开头，显式 vhost 才有非空前缀。
        if (!std::regex_match(url, std::regex("[^/]*/camera/[0-9a-f]{32}")) || client.value("alive", 0.0) < 20) continue;
        bool owned = false;
        for (const auto& item : rtc_connections_) owned = owned || item.second.client == id;
        for (const auto& item : rtc_clients_) owned = owned || item.second == id;
        // 业务进程重启会失去登录会话；不能让 SRS 中残留的连接继续绕过业务权限。
        if (!owned) co_await srs("DELETE", "/api/v1/clients/" + id);
    }
    // HTTP 回调可能在等待数据库时删除条目，跨 co_await 只持有副本。
    const std::vector<std::pair<std::string, RtcConnection>> snapshot(rtc_connections_.begin(), rtc_connections_.end());
    for (const auto& [handle, connection] : snapshot) {
        const auto session = connection.access.session.lock();
        bool remove = !session || !session->connected() ||
            ChatService::instance()->actor(session) != connection.access.actor;
        if (!present.contains(connection.client) &&
            std::chrono::steady_clock::now() - connection.created > std::chrono::seconds(20)) remove = true;
        if (!remove) {
            auto guard = co_await AsyncConnectionGuard::create();
            if (!guard->valid()) throw MediaError("unavailable");
            LiveRepository repository(guard->connection());
            try {
                const auto live = co_await repository.access(connection.access.actor,
                    connection.access.conversation, connection.access.id, connection.access.publish);
                remove = live["state"] == "ended";
            } catch (const MediaError& error) {
                const std::string code(error.what());
                if (code != "forbidden" && code != "not_found" && code != "unauthorized") throw;
                remove = true;
            }
        }
        if (remove) {
            const auto upstream = co_await rtc_upstream(http::verb::delete_, connection.resource);
            if (upstream.result_int() / 100 == 2) rtc_connections_.erase(handle);
        }
    }
}
} // namespace chat::media
