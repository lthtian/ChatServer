#pragma once

// 网络模块

#include <boost/asio.hpp>
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>
#include <chrono>

namespace asio = boost::asio;
using asio::ip::tcp;

#include "json.hpp"
using json = nlohmann::json;

#include "session.hpp"
#include "chatservice.hpp"

// 任务结构体：封装需要业务线程处理的消息
struct Task {
    Session::Ptr session;     // 客户端连接
    json js;                  // 解析后的JSON消息
    Timestamp time;           // 时间戳
    MsgHandler handler;       // 业务处理函数
};

// 线程安全的任务队列（生产者-消费者模型）
template<typename T>
class TaskQueue {
public:
    TaskQueue() : _shutdown(false) {}

    void push(const T& task) {
        std::lock_guard<std::mutex> lock(_mutex);
        _queue.push(task);
        _cond.notify_one();
    }

    T pop() {
        std::unique_lock<std::mutex> lock(_mutex);
        while (_queue.empty() && !_shutdown) {
            _cond.wait(lock);
        }
        if (_shutdown && _queue.empty()) {
            return T();
        }
        T task = _queue.front();
        _queue.pop();
        return task;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(_mutex);
        _shutdown = true;
        _cond.notify_all();
    }

    bool isShutdownAndEmpty() {
        std::lock_guard<std::mutex> lock(_mutex);
        return _shutdown && _queue.empty();
    }

private:
    std::queue<T> _queue;
    std::mutex _mutex;
    std::condition_variable _cond;
    bool _shutdown;
};

// 业务线程池
class WorkerThreadPool {
public:
    WorkerThreadPool(int threadNum, TaskQueue<Task>& queue)
        : _taskQueue(queue), _running(true) {
        for (int i = 0; i < threadNum; ++i) {
            _workers.emplace_back(std::thread(&WorkerThreadPool::workerFunc, this));
        }
    }

    ~WorkerThreadPool() {
        shutdown();
    }

    void shutdown() {
        if (_running) {
            _running = false;
            _taskQueue.shutdown();
            for (auto& worker : _workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
        }
    }

private:
    void workerFunc() {
        while (_running) {
            Task task = _taskQueue.pop();
            if (!task.handler) {
                break;
            }
            if (!task.session || !task.session->connected()) {
                continue;
            }
            try {
                task.handler(task.session, task.js, task.time);
            } catch (const std::exception& e) {
                std::cerr << "Worker thread exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Worker thread unknown exception" << std::endl;
            }
        }
    }

    std::vector<std::thread> _workers;
    TaskQueue<Task>& _taskQueue;
    std::atomic<bool> _running;
};

class ChatServer
{
public:
    ChatServer(asio::io_context &ioc, const std::string &ip, uint16_t port);
    ~ChatServer();
    void start();

private:
    asio::awaitable<void> do_accept();

    asio::io_context &ioc_;
    tcp::acceptor acceptor_;

    TaskQueue<Task> _taskQueue;
    WorkerThreadPool *_workerPool;
};
