#pragma once

#include "chat/media/disk_store.h"
#include "json.hpp"
#include <boost/asio.hpp>
#include <atomic>
#include <string>

namespace chat::media {
// 单机后台任务执行器。数据库保存进度，独立进程承担编解码和内存开销。
class MediaJobs {
public:
    // 从 CHAT_MEDIA_PROCESS 读取 Linux 程序绝对路径；未配置时不启动执行器。
    MediaJobs(boost::asio::any_io_executor executor, const std::string& root);
    ~MediaJobs();  // 停止子进程并等待磁盘工作线程退出。
    void start();  // 启动恢复、领取、执行、提交结果的协程。
    void stop();  // 发出停止信号；中断任务留给下次启动恢复。
    bool busy(const std::string& id) const { return running_id_ == id; }  // 防止处理中删除原件。
private:
    boost::asio::awaitable<void> run();  // 不持有数据库连接等待转码。
    std::string process(const nlohmann::json& job);  // 在线程池中运行子进程并校验、发布产物。
    void publish(const std::string& id, const std::string& hash,
                 const std::filesystem::path& folder);  // 最后一步原子公开 catalog。

    boost::asio::steady_timer timer_;  // 空闲或基础设施故障时等待，避免忙轮询。
    boost::asio::thread_pool worker_{1};  // 与 HTTP 磁盘线程分离；最多一个媒体任务。
    DiskStore store_;  // 原件与派生产物的受控对象存储。
    std::filesystem::path work_root_;  // 执行器的锁目录，不通过 HTTP 暴露。
    std::string program_;  // 受信任配置中的可执行文件，不接受客户端指定。
    std::string segment_type_;  // ts 或 fmp4，由服务端统一配置。
    std::string running_id_;  // 仅在聊天事件循环线程读写。
    std::atomic<bool> stopping_{false};  // 将关停请求传递给磁盘工作线程。
};
}
