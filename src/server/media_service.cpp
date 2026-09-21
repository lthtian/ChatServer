// ABOUTME: Streams private image bytes using bounded HTTP transfers and expiring tickets.
// ABOUTME: Validates hashes and decoded images before allowing message publication.
#include "media_service.hpp"
#include "async_connectionpool.hpp"
#include "chatservice.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <array>
#include <functional>

namespace chat::media {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using json = nlohmann::json;
using tcp = asio::ip::tcp;
namespace {
constexpr std::uint64_t kLimit = 20 * 1024 * 1024;
constexpr std::uint64_t kFileLimit = 100 * 1024 * 1024;
std::string Hex(const unsigned char* bytes, std::size_t count) {
    std::string result;
    for (std::size_t i = 0; i < count; ++i) {
        result += "0123456789abcdef"[bytes[i] >> 4];
        result += "0123456789abcdef"[bytes[i] & 15];
    }
    return result;
}
std::string Random(std::size_t count) {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(count)) != 1) throw MediaError("unavailable");
    return Hex(bytes.data(), count);
}
template<class Result>
asio::awaitable<Result> Work(std::function<Result()> function) {
    co_return function();
}
struct Cleanup {
    std::function<void()> function;
    ~Cleanup() { function(); }
};
Conversation ParseConversation(const json& value) {
    return {value.at("is_group").get<bool>(), value.at("target").get<int>()};
}
asio::awaitable<void> Reply(beast::tcp_stream& stream, http::status status, json value) {
    http::response<http::string_body> response(status, 11);
    response.set(http::field::content_type, "application/json");
    response.set(http::field::cache_control, "no-store");
    response.keep_alive(false);
    response.body() = value.dump();
    response.prepare_payload();
    stream.expires_after(std::chrono::seconds(15));
    co_await http::async_write(stream, response, asio::use_awaitable);
}
}

MediaService::MediaService(asio::any_io_executor executor, std::string root, int port, std::string url)
    : acceptor_(executor, tcp::endpoint(asio::ip::make_address("127.0.0.1"), port)),
      collection_timer_(executor), store_(root), url_(std::move(url)) {}

void MediaService::start() {
    asio::co_spawn(acceptor_.get_executor(), accept(), asio::detached);
    asio::co_spawn(acceptor_.get_executor(), collect(), asio::detached);
}

void MediaService::stop() {
    stopping_ = true;
    boost::system::error_code ignored;
    acceptor_.close(ignored);
    collection_timer_.cancel(ignored);
    for (auto* stream : streams_) stream->socket().close(ignored);
}

asio::awaitable<void> MediaService::collect() {
    while (!stopping_) {
        try {
            co_await asio::co_spawn(workers_, Work<bool>([this] { store_.CollectStaging(); return true; }), asio::use_awaitable);
            auto guard = co_await AsyncConnectionGuard::create();
            if (!guard->valid()) throw MediaError("unavailable");
            MediaRepository repository(guard->connection());
            const auto records = co_await repository.garbage();
            for (const auto& record : records) {
                const auto id = record["id"].get<std::string>();
                const int owner = record["owner"];
                if (active_.contains(id)) continue;
                active_.insert(id);
                Cleanup release{[this, id] { active_.erase(id); }};
                const auto media = co_await repository.owned(owner, id);
                if (media["state"] != "canceled" && media["expired"] == 0) continue;
                co_await asio::co_spawn(workers_, Work<bool>([this, id] {
                    store_.Remove(id + "/original"); store_.Remove(id + "/thumbnail"); return true;
                }), asio::use_awaitable);
                co_await repository.purged(owner, id);
            }
        } catch (const std::exception&) {
            std::cerr << "[MEDIA] Object cleanup deferred" << std::endl;
        }
        if (stopping_) co_return;
        collection_timer_.expires_after(std::chrono::minutes(10));
        co_await collection_timer_.async_wait(asio::use_awaitable);
    }
}

json MediaService::descriptor(Ticket ticket) {
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(tickets_, [now](const auto& item) { return item.second.deadline <= now; });
    if (tickets_.size() >= 1024) throw MediaError("busy");
    const auto token = Random(32);
    const auto method = ticket.upload ? "PUT" : "GET";
    tickets_.emplace(token, std::move(ticket));
    return {{"method", method}, {"url", url_ + "/media"},
        {"headers", {{"Authorization", "Bearer " + token}}},
        {"expires_at", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() + 300000}};
}

asio::awaitable<json> MediaService::request(int actor, Session::Ptr session, json input) {
    auto guard = co_await AsyncConnectionGuard::create();
    if (!guard->valid()) throw MediaError("unavailable");
    MediaRepository repository(guard->connection());
    const auto op = input.at("op").get<std::string>();
    if (op == "send_text") co_return co_await repository.send_text(actor,
        ParseConversation(input.at("conversation")), input.at("client_msg_id"), input.at("text"));
    if (op == "sync") co_return co_await repository.sync(actor,
        ParseConversation(input.at("conversation")), input.at("direction"),
        input.value("cursor", std::int64_t{0}), input.value("through", std::int64_t{-1}), input.value("limit", 50));
    if (op == "history") co_return co_await repository.history(actor,
        ParseConversation(input.at("conversation")), input.value("before_id", 0), input.value("limit", 50));
    if (op == "publish") co_return co_await repository.publish(actor,
        ParseConversation(input.at("conversation")), input.at("client_msg_id"), input.at("media_id"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    if (op == "read") {
        const auto conversation = ParseConversation(input.at("conversation"));
        const auto variant = input.at("variant").get<std::string>();
        if (variant != "original" && variant != "thumbnail") throw MediaError("invalid_argument");
        const auto media = co_await repository.access(actor, conversation, input.at("media_id"));
        if (media["kind"] == "file" && variant != "original") throw MediaError("invalid_argument");
        const bool original = variant == "original";
        co_return json{{"download", descriptor({actor, session, conversation, media["media_id"], variant, false, deadline})},
            {"bytes", media[original ? "actual_bytes" : "thumbnail_bytes"]},
            {"sha256", original ? media["sha256"] : json(nullptr)},
            {"mime", media[original ? "mime" : "thumbnail_mime"]}};
    }
    if (op == "cancel") {
        const auto id = input.at("media_id").get<std::string>();
        co_await repository.cancel(actor, id);
        if (!active_.contains(id)) {
            co_await asio::co_spawn(workers_, Work<bool>([this, id] {
                store_.Remove(id + "/original"); store_.Remove(id + "/thumbnail"); return true;
            }), asio::use_awaitable);
        }
        co_return json::object();
    }
    json media;
    if (op == "begin" || op == "begin_file") {
        UploadIntent intent{ParseConversation(input.at("conversation")), input.at("client_msg_id"),
            input.at("bytes").get<std::uint64_t>(), input.at("sha256")};
        if (op == "begin_file") { intent.kind = "file"; intent.name = input.at("name").get<std::string>(); }
        media = co_await repository.begin(actor, intent, Random(16), "disk");
    } else if (op == "status" || op == "retry") {
        media = co_await repository.owned(actor, input.at("media_id"));
        if (op == "retry") {
            const auto id = media["media_id"].get<std::string>();
            if (active_.contains(id)) throw MediaError("busy");
            if (media["state"] == "failed" || media["state"] == "processing" ||
                (media["expired"] != 0 && media["state"] != "canceled")) {
                active_.insert(id);
                Cleanup cleanup{[this, id] { active_.erase(id); }};
                if (media["state"] != "ready") {
                    co_await asio::co_spawn(workers_, Work<bool>([this, id] {
                        store_.Remove(id + "/original"); store_.Remove(id + "/thumbnail"); return true;
                    }), asio::use_awaitable);
                }
                co_await repository.retry(actor, id);
                media = co_await repository.owned(actor, id);
            }
        }
    } else throw MediaError("invalid_argument");
    json result = {{"media_id", media["media_id"]}, {"state", media["state"]},
        {"failure_code", media["failure_code"]}, {"expired", media["expired"]}};
    if (media["state"] == "uploading" && media["expired"] == 0) {
        result["upload"] = descriptor({actor, session, {}, media["media_id"], "original", true, deadline});
    }
    co_return result;
}

asio::awaitable<void> MediaService::accept() {
    while (true) {
        auto socket = co_await acceptor_.async_accept(asio::use_awaitable);
        if (connections_ >= 16) { socket.close(); continue; }
        ++connections_;
        asio::co_spawn(acceptor_.get_executor(), serve(std::move(socket)), asio::detached);
    }
}

asio::awaitable<void> MediaService::serve(tcp::socket socket) {
    Cleanup release{[this] { --connections_; }};
    beast::tcp_stream stream(std::move(socket));
    streams_.insert(&stream);
    Cleanup untrack{[this, &stream] { streams_.erase(&stream); }};
    std::string error;
    bool response_started = false;
    try {
        beast::flat_buffer buffer(65536);
        http::request_parser<http::buffer_body> parser;
        parser.header_limit(8192); parser.body_limit(kFileLimit);
        stream.expires_after(std::chrono::seconds(30));
        co_await http::async_read_header(stream, buffer, parser, asio::use_awaitable);
        const auto auth = std::string(parser.get()[http::field::authorization]);
        if (parser.get().target() != "/media" || !auth.starts_with("Bearer ")) throw MediaError("unauthorized");
        const auto found = tickets_.find(auth.substr(7));
        if (found == tickets_.end() || found->second.deadline <= std::chrono::steady_clock::now()) throw MediaError("expired_ticket");
        const auto ticket = found->second;
        const auto session = ticket.session.lock();
        if (!session || !session->connected() || ChatService::instance()->actor(session) != ticket.actor) throw MediaError("unauthorized");
        if (parser.get().method() != (ticket.upload ? http::verb::put : http::verb::get)) throw MediaError("invalid_argument");
        if (ticket.upload) tickets_.erase(found);
        json media;
        {
            auto guard = co_await AsyncConnectionGuard::create();
            if (!guard->valid()) throw MediaError("unavailable");
            MediaRepository repository(guard->connection());
            if (ticket.upload) media = co_await repository.owned(ticket.actor, ticket.media_id);
            else media = co_await repository.access(ticket.actor, ticket.conversation, ticket.media_id);
        }
        if (ticket.upload) {
            if (active_.size() >= 2 || active_.contains(ticket.media_id)) throw MediaError("busy");
            if (media["state"] != "uploading" || media["expired"] != 0) throw MediaError("state_conflict");
            if (!parser.content_length() || *parser.content_length() != media["expected_bytes"].get<std::uint64_t>()) throw MediaError("size_mismatch");
            co_await upload(stream, buffer, parser, ticket, media);
            response_started = true;
            json result = {{"ok", true}};
            co_await Reply(stream, http::status::ok, std::move(result));
        } else {
            response_started = true;
            co_await download(stream, ticket, media);
        }
    } catch (const MediaError& failure) { error = failure.what(); }
      catch (const ImageError&) { error = "invalid_image"; }
      catch (const std::exception&) { error = "transfer_failed"; }
    if (!error.empty() && !response_started) {
        json result = {{"ok", false}, {"error", error}};
        try { co_await Reply(stream, http::status::bad_request, std::move(result)); }
        catch (...) {}
    }
    boost::system::error_code ignored;
    stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
}

asio::awaitable<void> MediaService::upload(beast::tcp_stream& stream, beast::flat_buffer& buffer,
    http::request_parser<http::buffer_body>& parser, Ticket ticket, json media) {
    const auto id = ticket.media_id;
    active_.insert(id);
    Cleanup release{[this, id] { active_.erase(id); }};
    std::exception_ptr failure;
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(30);
        auto upload = co_await asio::co_spawn(workers_, Work<std::shared_ptr<DiskUpload>>([this, media, id] {
            return store_.Begin(id + "/original", media["expected_bytes"], media["kind"] == "file" ? kFileLimit : kLimit);
        }), asio::use_awaitable);
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        if (!hash || EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) != 1) throw MediaError("unavailable");
        std::array<std::uint8_t, 65536> bytes{};
        while (!parser.is_done()) {
            parser.get().body().data = bytes.data(); parser.get().body().size = bytes.size();
            stream.expires_at(std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(30)));
            boost::system::error_code ec;
            co_await http::async_read_some(stream, buffer, parser, asio::redirect_error(asio::use_awaitable, ec));
            if (ec && ec != http::error::need_buffer) throw boost::system::system_error(ec);
            const auto count = bytes.size() - parser.get().body().size;
            if (EVP_DigestUpdate(hash.get(), bytes.data(), count) != 1) throw MediaError("unavailable");
            co_await asio::co_spawn(workers_, Work<bool>([upload, &bytes, count] {
                upload->Append(std::span(bytes.data(), count)); return true;
            }), asio::use_awaitable);
        }
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{}; unsigned count = 0;
        if (EVP_DigestFinal_ex(hash.get(), digest.data(), &count) != 1) throw MediaError("unavailable");
        if (Hex(digest.data(), count) != media["sha256"].get<std::string>()) throw MediaError("hash_mismatch");
        {
            auto guard = co_await AsyncConnectionGuard::create();
            if (!guard->valid()) throw MediaError("unavailable");
            MediaRepository repository(guard->connection());
            if (!co_await repository.start_processing(ticket.actor, id)) throw MediaError("state_conflict");
        }
        if (media["kind"] == "file") {
            co_await asio::co_spawn(workers_, Work<bool>([upload] { upload->Commit(); return true; }), asio::use_awaitable);
            auto guard = co_await AsyncConnectionGuard::create();
            if (!guard->valid()) throw MediaError("unavailable");
            MediaRepository repository(guard->connection());
            co_await repository.ready_file(ticket.actor, id, media["expected_bytes"]);
        } else {
        auto thumbnail = co_await asio::co_spawn(workers_, Work<Thumbnail>([this, upload, id] {
            upload->Commit();
            auto input = store_.Open(id + "/original");
            const std::vector<std::uint8_t> encoded{std::istreambuf_iterator<char>(input), {}};
            auto thumbnail = CreateThumbnail(encoded);
            auto output = store_.Begin(id + "/thumbnail", thumbnail.bytes.size(), kLimit);
            output->Append(thumbnail.bytes); output->Commit();
            return thumbnail;
        }), asio::use_awaitable);
        auto guard = co_await AsyncConnectionGuard::create();
        if (!guard->valid()) throw MediaError("unavailable");
        MediaRepository repository(guard->connection());
        co_await repository.ready(ticket.actor, id, std::move(thumbnail), media["expected_bytes"]);
        }
    } catch (...) { failure = std::current_exception(); }
    if (failure) {
        try {
            co_await asio::co_spawn(workers_, Work<bool>([this, id] {
                store_.Remove(id + "/original"); store_.Remove(id + "/thumbnail"); return true;
            }), asio::use_awaitable);
            auto guard = co_await AsyncConnectionGuard::create();
            if (guard->valid()) {
                MediaRepository repository(guard->connection());
                co_await repository.fail(ticket.actor, id, "upload_failed");
            }
        } catch (...) {}
        std::rethrow_exception(failure);
    }
}

asio::awaitable<void> MediaService::download(beast::tcp_stream& stream, Ticket ticket, json media) {
    const bool original = ticket.variant == "original";
    const auto key = media[original ? "original_key" : "thumbnail_key"].get<std::string>();
    auto file = co_await asio::co_spawn(workers_, Work<std::shared_ptr<std::ifstream>>([this, key] {
        return std::make_shared<std::ifstream>(store_.Open(key));
    }), asio::use_awaitable);
    http::response<http::empty_body> response(http::status::ok, 11);
    response.set(http::field::content_type, media[original ? "mime" : "thumbnail_mime"].get<std::string>());
    response.set(http::field::cache_control, "private, no-store");
    response.set("X-Content-Type-Options", "nosniff");
    if (media["kind"] == "file") response.set(http::field::content_disposition, "attachment");
    response.content_length(media[original ? "actual_bytes" : "thumbnail_bytes"].get<std::uint64_t>());
    response.keep_alive(false);
    http::response_serializer<http::empty_body> serializer(response);
    stream.expires_after(std::chrono::seconds(30));
    co_await http::async_write_header(stream, serializer, asio::use_awaitable);
    std::array<char, 65536> bytes{};
    while (true) {
        const auto count = co_await asio::co_spawn(workers_, Work<std::size_t>([file, &bytes] {
            file->read(bytes.data(), bytes.size());
            if (file->bad()) throw MediaError("io_error");
            return file->gcount();
        }), asio::use_awaitable);
        if (!count) break;
        stream.expires_after(std::chrono::seconds(30));
        co_await asio::async_write(stream, asio::buffer(bytes.data(), count), asio::use_awaitable);
    }
}
}
