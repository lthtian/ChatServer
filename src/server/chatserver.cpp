#include "chatserver.hpp"
#include "chatservice.hpp"
#include <iostream>
using namespace std;

ChatServer::ChatServer(asio::io_context &ioc, const std::string &ip, uint16_t port)
    : ioc_(ioc),
      acceptor_(ioc, tcp::endpoint(asio::ip::make_address(ip), port)),
      _workerPool(nullptr)
{
    // 创建业务线程池（线程数 = CPU核心数 - 2，至少保留2个给IO线程）
    int workerThreadNum = std::thread::hardware_concurrency() - 2;
    if (workerThreadNum < 4) {
        workerThreadNum = 4;
    }
    _workerPool = new WorkerThreadPool(workerThreadNum, _taskQueue);
    cout << "Business thread pool started with " << workerThreadNum << " threads." << endl;
}

ChatServer::~ChatServer()
{
    if (_workerPool) {
        _workerPool->shutdown();
        delete _workerPool;
        _workerPool = nullptr;
    }

    boost::system::error_code ec;
    acceptor_.close(ec);
}

void ChatServer::start()
{
    asio::co_spawn(ioc_, do_accept(), asio::detached);
}

asio::awaitable<void> ChatServer::do_accept()
{
    while (true)
    {
        auto socket = co_await acceptor_.async_accept(asio::use_awaitable);

        auto session = std::make_shared<Session>(
            std::move(socket),
            // MessageCallback: 解析消息并投递到业务线程池
            [this](const Session::Ptr &s, json js) {
                auto handler = ChatService::instance()->getHandler(js["msgid"].get<int>());
                Task task;
                task.session = s;
                task.js = std::move(js);
                task.time = std::chrono::steady_clock::now();
                task.handler = handler;
                _taskQueue.push(task);
            },
            // CloseCallback: 连接断开时通知业务层
            [](const Session::Ptr &s) {
                ChatService::instance()->clientCloseException(s);
            }
        );
        session->start();
    }
}
