#pragma once

#include "json.hpp"
#include <boost/asio.hpp>
#include <boost/mysql.hpp>
#include <functional>
#include <string_view>
#include <vector>

namespace chat::media::db {
namespace asio = boost::asio;
namespace mysql = boost::mysql;
using Connection = mysql::tcp_connection;
using json = nlohmann::json;

// 连接状态不可信时关闭底层 socket，不将其作为可用连接交还池。
inline void Disconnect(Connection& connection) {
    boost::system::error_code ignored;
    connection.stream().close(ignored);
}

// 绑定参数、执行并关闭语句；保留执行异常，避免清理错误掩盖原始原因。
inline asio::awaitable<mysql::results> Query(Connection& connection, std::string sql,
                                             std::vector<mysql::field> parameters) {
    auto statement = co_await connection.async_prepare_statement(sql, asio::use_awaitable);
    std::vector<mysql::field_view> views;
    for (const auto& parameter : parameters) views.emplace_back(parameter);
    mysql::results result;
    std::exception_ptr failure;
    try {
        co_await connection.async_execute(statement.bind(views.begin(), views.end()),
                                          result, asio::use_awaitable);
    } catch (...) { failure = std::current_exception(); }
    boost::system::error_code close_error;
    co_await connection.async_close_statement(statement,
        asio::redirect_error(asio::use_awaitable, close_error));
    if (close_error) Disconnect(connection);
    if (failure) std::rethrow_exception(failure);
    if (close_error) throw boost::system::system_error(close_error);
    co_return result;
}

// 将参数保存为拥有数据的 field，确保挂起期间绑定值仍然有效。
template<class... Args>
asio::awaitable<mysql::results> Execute(Connection& connection, std::string_view sql, Args&&... args) {
    return Query(connection, std::string(sql),
                 std::vector<mysql::field>{mysql::field(std::forward<Args>(args))...});
}

// 事务只围绕数据库工作；异常回滚，提交成功后才返回业务结果。
inline asio::awaitable<json> Transaction(Connection& connection,
                                        std::function<asio::awaitable<json>()> body) {
    mysql::results result;
    co_await connection.async_execute("START TRANSACTION", result, asio::use_awaitable);
    std::exception_ptr failure;
    json output;
    try {
        output = co_await body();
        co_await connection.async_execute("COMMIT", result, asio::use_awaitable);
    } catch (...) { failure = std::current_exception(); }
    if (failure) {
        boost::system::error_code error;
        co_await connection.async_execute("ROLLBACK", result,
            asio::redirect_error(asio::use_awaitable, error));
        if (error) Disconnect(connection);
        std::rethrow_exception(failure);
    }
    co_return output;
}

// 将数据库标量转为 JSON；数据库字符串在返回前复制。
inline json Value(mysql::field_view field) {
    if (field.is_null()) return nullptr;
    if (field.is_int64()) return field.as_int64();
    if (field.is_uint64()) return field.as_uint64();
    return std::string(field.as_string());
}
}
