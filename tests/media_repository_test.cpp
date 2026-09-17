// ABOUTME: Exercises media state and message transactions against a real MySQL server.
// ABOUTME: Requires a disposable test database and verifies permission and retry behavior.
// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/09/16 16:56
#include "media_repository.hpp"
#include <cstdlib>
#include <iostream>

namespace {
namespace asio = boost::asio;
namespace mysql = boost::mysql;
using namespace chat::media;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::string Environment(const char* key) {
    const auto* value = std::getenv(key);
    if (!value || !*value) throw std::runtime_error("Test connection environment missing");
    return value;
}

asio::awaitable<void> Test(mysql::tcp_connection& connection, const std::string scenario) {
    const auto database = Environment("CHAT_TEST_DATABASE");
    Require(database.starts_with("chat_media_test_"), "A disposable database is required");
    const auto user = Environment("CHAT_TEST_USER");
    const auto password = Environment("CHAT_TEST_PASSWORD");
    mysql::handshake_params parameters(user, password, database);
    co_await connection.async_connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 3306),
                                      parameters, asio::use_awaitable);
    MediaRepository repository(connection);
    if (scenario == "send") {
        const auto sent = co_await repository.publish(1, Conversation{false, 2},
            "00000000-0000-4000-8000-000000000004", std::string(31, 'a') + '4');
        Require(sent.at("message").at("media").at("media_id") == std::string(31, 'a') + '4',
                "Concurrent retry returned another image");
        co_await connection.async_close(asio::use_awaitable);
        co_return;
    }
    const int number = scenario == "lifecycle" ? 1 : scenario == "permissions" ? 2 :
                       scenario == "cancel" ? 3 : scenario == "prepare" ? 4 :
                       scenario == "failure" ? 5 : scenario == "expiry" ? 6 : 7;
    const auto media_id = std::string(31, 'a') + std::to_string(number);
    const auto client_id = "00000000-0000-4000-8000-00000000000" + std::to_string(number);
    const Conversation private_chat{scenario == "group", scenario == "group" ? 7 : 2};
    const UploadIntent intent{private_chat, client_id, 100, std::string(64, 'b')};
    const auto uploaded = co_await repository.begin(1, intent, media_id, "disk");
    Require(uploaded.at("state") == "uploading", "Upload did not start");
    const auto repeated = co_await repository.begin(1, intent, std::string(32, 'c'), "disk");
    Require(repeated.at("media_id") == media_id, "Upload retry created another media row");
    auto conflicting_intent = intent;
    conflicting_intent.bytes = 101;
    bool conflict = false;
    try { co_await repository.begin(1, conflicting_intent, std::string(32, 'd'), "disk"); }
    catch (const MediaError& error) { conflict = std::string(error.what()) == "conflict"; }
    Require(conflict, "An idempotency key was reused with another payload");

    if (scenario == "permissions") {
        bool refused = false;
        try { co_await repository.owned(2, media_id); }
        catch (const MediaError& error) { refused = std::string(error.what()) == "not_found"; }
        Require(refused, "Another user can read upload state");
        refused = false;
        try { co_await repository.history(3, private_chat, 0, 20); }
        catch (const MediaError& error) { refused = std::string(error.what()) == "forbidden"; }
        Require(refused, "A non-friend can read private history");
        refused = false;
        try { co_await repository.history(3, Conversation{true, 7}, 0, 20); }
        catch (const MediaError& error) { refused = std::string(error.what()) == "forbidden"; }
        Require(refused, "A non-member can read group history");
        refused = false;
        try { co_await repository.access(2, Conversation{false, 1}, media_id); }
        catch (const MediaError& error) { refused = std::string(error.what()) == "not_found"; }
        Require(refused, "An unfinished object is accessible to a recipient");
    } else if (scenario == "cancel") {
        co_await repository.cancel(1, media_id);
        const auto canceled = co_await repository.owned(1, media_id);
        Require(canceled.at("state") == "canceled", "Cancel did not persist");
        Require(!(co_await repository.start_processing(1, media_id)),
                "Canceled upload started processing");
    } else if (scenario == "failure") {
        Require(co_await repository.start_processing(1, media_id), "Processing did not start");
        co_await repository.fail(1, media_id, "invalid_image");
        const auto failed = co_await repository.owned(1, media_id);
        Require(failed.at("state") == "failed" && failed.at("failure_code") == "invalid_image",
                "Processing failure was not persisted");
        bool refused = false;
        try { co_await repository.publish(1, private_chat, client_id, media_id); }
        catch (const MediaError& error) { refused = std::string(error.what()) == "not_ready"; }
        Require(refused, "Failed media was published");
        co_await repository.retry(1, media_id);
        const auto retried = co_await repository.owned(1, media_id);
        Require(retried.at("state") == "uploading" && retried.at("failure_code").is_null(),
                "Failed upload could not be retried");
    } else if (scenario == "expiry") {
        mysql::results expired;
        co_await connection.async_execute("UPDATE Media SET expires_at=CURRENT_TIMESTAMP-INTERVAL 1 SECOND "
            "WHERE media_id='" + media_id + "'", expired, asio::use_awaitable);
        Require(!(co_await repository.start_processing(1, media_id)), "Expired media started processing");
        const auto garbage = co_await repository.garbage();
        bool found = false;
        for (const auto& item : garbage) if (item.at("id") == media_id) found = true;
        Require(found, "Expired unbound object was not collected");
        co_await repository.purged(1, media_id);
        co_await repository.retry(1, media_id);
        Require(co_await repository.start_processing(1, media_id), "Expired upload could not be renewed");
    } else {
        bool refused = false;
        try { co_await repository.publish(1, private_chat, client_id, media_id); }
        catch (const MediaError& error) { refused = std::string(error.what()) == "not_ready"; }
        Require(refused, "Unverified media was published");
        Require(co_await repository.start_processing(1, media_id), "Processing did not start");
        Require(!(co_await repository.start_processing(1, media_id)), "Processing started twice");
        Thumbnail thumbnail;
        thumbnail.source_mime = "image/png";
        thumbnail.source_width = 640;
        thumbnail.source_height = 320;
        thumbnail.mime = "image/png";
        thumbnail.width = 320;
        thumbnail.height = 160;
        thumbnail.bytes.resize(10);
        refused = false;
        try { co_await repository.ready(1, media_id, thumbnail, 99); }
        catch (const MediaError& error) { refused = std::string(error.what()) == "state_conflict"; }
        Require(refused, "An incomplete original was marked ready");
        co_await repository.ready(1, media_id, std::move(thumbnail), 100);
        if (scenario == "prepare") {
            co_await connection.async_close(asio::use_awaitable);
            co_return;
        }
        const auto sent = co_await repository.publish(1, private_chat, client_id, media_id);
        const auto retry = co_await repository.publish(1, private_chat, client_id, media_id);
        Require(sent.at("created") == true && retry.at("created") == false,
                "Message retry was not idempotent");
        Require(sent.at("message").at("message_id") == retry.at("message").at("message_id"),
                "Message retry changed its identifier");
        const Conversation recipient{private_chat.is_group, private_chat.is_group ? 7 : 1};
        const auto accessible = co_await repository.access(2, recipient, media_id);
        Require(accessible.at("media_id") == media_id, "Recipient cannot access sent media");
        const auto history = co_await repository.history(2, recipient, 0, 1);
        Require(history.at("messages").size() == 1, "Page limit was ignored");
        Require(history.at("messages").at(0).at("media").at("media_id") == media_id,
                "History lost its image metadata");
        if (!private_chat.is_group) {
            const auto earlier = co_await repository.history(2, recipient,
                std::stoi(history.at("next_cursor").get<std::string>()), 20);
            for (const auto& message : earlier.at("messages")) {
                Require(message.at("message_id") != sent.at("message").at("message_id"),
                        "History cursor repeated a message");
            }
        }
        refused = false;
        try { co_await repository.cancel(1, media_id); }
        catch (const MediaError& error) { refused = std::string(error.what()) == "already_sent"; }
        Require(refused, "A published image was canceled");
    }
    co_await connection.async_close(asio::use_awaitable);
}
}  // namespace

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "A scenario is required");
        asio::io_context io;
        mysql::tcp_connection connection(io);
        std::exception_ptr failure;
        asio::co_spawn(io, Test(connection, argv[1]),
            [&](std::exception_ptr error) { failure = error; });
        io.run();
        if (failure) std::rethrow_exception(failure);
        std::cout << "PASS " << argv[1] << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
