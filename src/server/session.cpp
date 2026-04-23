#include "session.hpp"
#include <iostream>

Session::Session(tcp::socket socket, MessageCallback msgCb, CloseCallback closeCb)
    : socket_(std::move(socket)),
      messageCallback_(std::move(msgCb)),
      closeCallback_(std::move(closeCb))
{
}

Session::~Session()
{
    close();
}

void Session::start()
{
    auto self = shared_from_this();
    asio::co_spawn(socket_.get_executor(),
        [self]() -> asio::awaitable<void> {
            co_await self->read_loop();
        },
        asio::detached);
}

void Session::send(const std::string &msg)
{
    auto self = shared_from_this();
    asio::post(socket_.get_executor(),
        [self, msg]()
        {
            if (self->closed_.load())
                return;
            self->writeQueue_.push(msg);
            if (self->writeQueue_.size() == 1)
            {
                self->do_write();
            }
        });
}

std::string Session::remote_endpoint() const
{
    boost::system::error_code ec;
    auto ep = socket_.remote_endpoint(ec);
    if (ec)
        return "unknown";
    return ep.address().to_string() + ":" + std::to_string(ep.port());
}

asio::awaitable<void> Session::read_loop()
{
    char read_buf[4096];
    try
    {
        while (true)
        {
            std::size_t n = co_await socket_.async_read_some(
                asio::buffer(read_buf), asio::use_awaitable);
            recvBuf_.append(read_buf, n);

            // Brace-matching JSON extraction (ported from original ChatServer::onMessage)
            size_t start_pos = 0;
            int brace_count = 0;
            bool in_string = false;

            for (size_t i = 0; i < recvBuf_.size(); ++i)
            {
                char c = recvBuf_[i];
                if (c == '"')
                {
                    int backslashCount = 0;
                    size_t j = i;
                    while (j > 0 && recvBuf_[j - 1] == '\\')
                    {
                        backslashCount++;
                        j--;
                    }
                    if (backslashCount % 2 == 0)
                    {
                        in_string = !in_string;
                    }
                }

                if (!in_string)
                {
                    if (c == '{')
                    {
                        brace_count++;
                    }
                    else if (c == '}')
                    {
                        brace_count--;
                        if (brace_count == 0)
                        {
                            std::string json_str = recvBuf_.substr(start_pos, i - start_pos + 1);
                            start_pos = i + 1;

                            try
                            {
                                json js = json::parse(json_str);
                                co_await messageCallback_(shared_from_this(), std::move(js));
                            }
                            catch (const std::exception &e)
                            {
                                std::cerr << "Handler error: " << e.what() << std::endl;
                                // 不关闭连接，继续处理下一条消息
                            }
                        }
                    }
                }
            }

            // Keep unprocessed data
            if (start_pos > 0)
            {
                recvBuf_ = (start_pos < recvBuf_.size()) ? recvBuf_.substr(start_pos) : "";
            }
        }
    }
    catch (const boost::system::system_error &e)
    {
        // Connection closed or error - expected when client disconnects
    }
    catch (const std::exception &e)
    {
        std::cerr << "Session read error: " << e.what() << std::endl;
    }

    close();
}

void Session::do_write()
{
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(writeQueue_.front()),
        [self](boost::system::error_code ec, std::size_t /*length*/)
        {
            if (ec)
            {
                self->close();
                return;
            }
            self->writeQueue_.pop();
            if (!self->writeQueue_.empty())
            {
                self->do_write();
            }
        });
}

void Session::close()
{
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true))
        return;

    if (closeCallback_)
    {
        closeCallback_(shared_from_this());
    }

    boost::system::error_code ec;
    socket_.close(ec);
}
