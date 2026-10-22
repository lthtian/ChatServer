#pragma once
#include "live_repository.hpp"
#include "session.hpp"
#include <boost/beast.hpp>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace chat::media {
// 直播业务控制；媒体字节由 SRS 发送，本类只处理授权、状态和内部回调。
class LiveService {
public:
    using Notify = std::function<boost::asio::awaitable<void>(nlohmann::json)>;
    explicit LiveService(boost::asio::any_io_executor executor);
    // 配置 CHAT_LIVE_URL 时启用，内部 HTTP 只监听 loopback。
    void start(Notify notify);
    void stop();  // 停止接收和状态核对；不终止其他进程。
    // 处理聊天连接上的 live_create/publish/read/status/end。
    boost::asio::awaitable<nlohmann::json> request(int actor, Session::Ptr session, nlohmann::json input);
private:
    struct Ticket {
        int actor;                              // 签发时的登录用户。
        std::weak_ptr<Session> session;          // 绑定登录会话，不能脱离登录长期使用。
        Conversation conversation;              // 绑定私聊或群聊权限。
        std::string id;                         // 唯一直播 ID，不接受任意上游路径。
        bool publish;                           // 推流票据与观看票据用途分离。
        std::chrono::steady_clock::time_point deadline;  // 新连接授权截止时间。
        bool rtc = false;                       // RTC 建连票据不能用于文件式直播读取。
    };
    struct RtcConnection {
        Ticket access;                          // 会话存续期间检查登录和会话权限。
        std::string token;                      // 删除会话的持有凭证，建立后不受建连期限限制。
        std::string resource;                   // SRS 返回的内部 DELETE 路径，不暴露给客户端。
        std::string client;                     // SRS 回调中的连接 ID，供状态核对。
        std::chrono::steady_clock::time_point created; // 容许 ICE/DTLS 建连的短暂窗口。
    };
    // 签发五分钟凭证；HTTP 播放走 /live/，RTC 建连走 /rtc/。
    nlohmann::json descriptor(int actor, const Session::Ptr& session, Conversation conversation,
        const std::string& id, bool publish, const std::string& format);
    // 校验 token、会话和期限；返回副本避免 co_await 期间迭代器失效。
    Ticket ticket(const std::string& token, bool publish) const;
    boost::asio::awaitable<void> accept();  // 为每个内部请求创建独立协程。
    boost::asio::awaitable<void> serve(boost::asio::ip::tcp::socket socket);
    // SRS 回调，secret 由部署配置提供；仅有效发布票据可改变状态。
    boost::asio::awaitable<void> hook(nlohmann::json input);
    // Nginx auth_request 子请求：每次读取列表/分片也重新检查业务权限。
    boost::asio::awaitable<void> authorize(std::string authorization, std::string path);
    // 只访问固定 loopback SRS API，不能由客户端指定 URL。
    boost::asio::awaitable<nlohmann::json> srs(std::string method, std::string path);
    // 固定本机 SRS 上游：交换 SDP 或删除 RTC 资源，不转发任意 URL。
    boost::asio::awaitable<boost::beast::http::response<boost::beast::http::string_body>>
        rtc_upstream(boost::beast::http::verb method, std::string path, std::string body = {});
    // 公网 /rtc/ 入口经 Nginx 转入此处，先校验业务票据再进行 WHIP/WHEP。
    boost::asio::awaitable<boost::beast::http::response<boost::beast::http::string_body>>
        rtc_request(const boost::beast::http::request<boost::beast::http::string_body>& request);
    boost::asio::awaitable<void> prune_rtc(); // 登录退出、权限撤回、直播结束或媒体连接消失时清理。
    boost::asio::awaitable<void> monitor();  // 回调丢失或 SRS 重启时核对活跃流。
    boost::asio::ip::tcp::acceptor acceptor_;  // 仅内部控制 HTTP，不承载媒体数据。
    boost::asio::steady_timer timer_;         // 三秒核对一次状态。
    Notify notify_;                          // 状态变化交给聊天服务通知参与者。
    std::unordered_map<std::string, Ticket> tickets_;  // 有界短期凭证表。
    std::unordered_map<std::string, RtcConnection> rtc_connections_; // 公开随机资源 ID 到内部会话的映射。
    std::unordered_set<std::string> rtc_opening_;       // 防止同一建连票据并发重复使用。
    std::unordered_map<std::string, std::string> rtc_clients_; // SDP 请求期间由 SRS 回调填入连接 ID。
    std::string url_;                         // 对外 HTTP 根地址，带 /live。
    std::string rtc_url_;                     // 对外 RTC 信令根地址，带 /rtc；为空时禁止 RTC 创建。
    std::string rtmp_url_;                    // 对外 RTMP 应用地址，带 /live。
    std::string secret_;                      // SRS 回调共享密钥，不返回客户端。
    unsigned short api_port_ = 1985;          // SRS 内部管理 API 端口。
    std::size_t connections_ = 0;             // 限制内部 HTTP 并发为 32。
    bool stopping_ = false;                   // 停止后不再发起新任务。
};
}
