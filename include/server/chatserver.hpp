#pragma once

// 网络模块

#include <muduo/net/TcpServer.h>
#include <muduo/net/EventLoop.h>
#include <muduo/base/Timestamp.h>
#include <functional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>

using namespace muduo;
using namespace muduo::net;

#include "json.hpp"
using json = nlohmann::json;

#include "chatservice.hpp"

// 任务结构体：封装需要业务线程处理的消息
struct Task {
    TcpConnectionPtr conn;    // 客户端连接
    json js;                  // 解析后的JSON消息
    Timestamp time;           // 时间戳
    MsgHandler handler;       // 业务处理函数
};

// 线程安全的任务队列（生产者-消费者模型）
template<typename T>
class TaskQueue {
public:
    TaskQueue() : _shutdown(false) {}

    // 生产者：将任务放入队列
    void push(const T& task) {
        std::lock_guard<std::mutex> lock(_mutex);
        _queue.push(task);
        _cond.notify_one();  // 通知一个等待的消费者
    }

    // 消费者：从队列取出任务（阻塞式）
    T pop() {
        std::unique_lock<std::mutex> lock(_mutex);
        // 等待队列非空或收到关闭信号
        while (_queue.empty() && !_shutdown) {
            _cond.wait(lock);
        }
        // 如果收到关闭信号且队列空，返回空任务
        if (_shutdown && _queue.empty()) {
            return T();
        }
        T task = _queue.front();
        _queue.pop();
        return task;
    }

    // 关闭队列，唤醒所有等待的线程
    void shutdown() {
        std::lock_guard<std::mutex> lock(_mutex);
        _shutdown = true;
        _cond.notify_all();
    }

    // 检查队列是否已关闭且为空
    bool isShutdownAndEmpty() {
        std::lock_guard<std::mutex> lock(_mutex);
        return _shutdown && _queue.empty();
    }

private:
    std::queue<T> _queue;              // 任务队列
    std::mutex _mutex;                 // 互斥锁
    std::condition_variable _cond;     // 条件变量
    bool _shutdown;                    // 关闭标志
};

// 业务线程池
class WorkerThreadPool {
public:
    WorkerThreadPool(int threadNum, TaskQueue<Task>& queue)
        : _taskQueue(queue), _running(true) {
        // 创建指定数量的工作线程
        for (int i = 0; i < threadNum; ++i) {
            _workers.emplace_back(std::thread(&WorkerThreadPool::workerFunc, this));
        }
    }

    ~WorkerThreadPool() {
        shutdown();
    }

    // 关闭线程池
    void shutdown() {
        if (_running) {
            _running = false;
            _taskQueue.shutdown();  // 先关闭任务队列
            // 等待所有工作线程结束
            for (auto& worker : _workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
        }
    }

private:
    // 工作线程函数：循环从队列取任务并执行
    void workerFunc() {
        while (_running) {
            Task task = _taskQueue.pop();
            // 检查是否为空任务（关闭信号）
            if (!task.handler) {
                break;
            }
            // 检查连接是否仍然有效
            if (!task.conn || !task.conn->connected()) {
                continue;  // 连接已断开，跳过此任务
            }
            try {
                // 执行业务处理函数
                task.handler(task.conn, task.js, task.time);
            } catch (const std::exception& e) {
                std::cerr << "Worker thread exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Worker thread unknown exception" << std::endl;
            }
        }
    }

    std::vector<std::thread> _workers;  // 工作线程集合
    TaskQueue<Task>& _taskQueue;        // 任务队列引用
    std::atomic<bool> _running;         // 运行标志
};

class ChatServer
{
public:
    ChatServer(EventLoop *loop, const InetAddress &listenAddr, const string &nameArg);
    ~ChatServer();  // 析构函数：确保线程池正确关闭
    void start();

private:
    void onConnection(const TcpConnectionPtr &);
    void onMessage(const TcpConnectionPtr &, Buffer *, Timestamp);

    TcpServer _server;
    EventLoop *_loop;

    std::unordered_map<TcpConnectionPtr, std::string> _recvBuffers;

    // 业务线程池相关成员
    TaskQueue<Task> _taskQueue;           // 任务队列
    WorkerThreadPool *_workerPool;        // 业务线程池（动态创建）
};