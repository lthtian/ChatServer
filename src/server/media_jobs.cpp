#include "media_jobs.hpp"
#include "async_connectionpool.hpp"
#include "hls_catalog.hpp"
#include "media_repository.hpp"
#include <openssl/evp.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace chat::media {
namespace asio = boost::asio;
namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
template<class T> asio::awaitable<T> Work(std::function<T()> action) { co_return action(); }
struct FileLock {
    int descriptor;
    ~FileLock() { close(descriptor); }
};

std::string Digest(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!file || !context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
        throw MediaError("io_error");
    std::array<char, 65536> bytes{};
    while (file.read(bytes.data(), bytes.size()) || file.gcount())
        if (EVP_DigestUpdate(context.get(), bytes.data(), file.gcount()) != 1) throw MediaError("hash_failed");
    if (!file.eof()) throw MediaError("io_error");
    unsigned size = 0;
    unsigned char digest[EVP_MAX_MD_SIZE];
    if (EVP_DigestFinal_ex(context.get(), digest, &size) != 1) throw MediaError("hash_failed");
    std::string result;
    for (unsigned i = 0; i < size; ++i) {
        result += "0123456789abcdef"[digest[i] >> 4];
        result += "0123456789abcdef"[digest[i] & 15];
    }
    return result;
}

// argv 直接交给操作系统，不经过 shell；文件名无法变成命令。
std::string Execute(const std::string& program, const std::string& source,
                    const std::string& output, const std::string& type,
                    const std::string& log, const std::atomic<bool>& stopping) {
    posix_spawn_file_actions_t files;
    posix_spawnattr_t attributes;
    posix_spawn_file_actions_init(&files);
    posix_spawnattr_init(&attributes);
    int error = posix_spawn_file_actions_addopen(&files, STDOUT_FILENO, log.c_str(),
                                               O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (!error) error = posix_spawn_file_actions_adddup2(&files, STDOUT_FILENO, STDERR_FILENO);
    sigset_t defaults, mask;
    sigemptyset(&defaults); sigaddset(&defaults, SIGINT); sigaddset(&defaults, SIGTERM);
    sigemptyset(&mask);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setsigmask(&attributes, &mask);
    posix_spawnattr_setpgroup(&attributes, 0);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    std::array<char*, 6> args{const_cast<char*>(program.c_str()), const_cast<char*>("hls"),
        const_cast<char*>(source.c_str()), const_cast<char*>(output.c_str()), const_cast<char*>(type.c_str()), nullptr};
    pid_t pid = -1;
    if (!error) error = posix_spawn(&pid, program.c_str(), &files, &attributes, args.data(), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&files);
    if (error) return "processor_start_failed";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(30);
    int status = 0;
    while (true) {
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "" : "transcode_failed";
        if (result < 0 && errno != EINTR) return "processor_wait_failed";
        if (stopping || std::chrono::steady_clock::now() >= deadline) {
            kill(-pid, SIGTERM);
            for (int i = 0; i < 20; ++i) {
                if (waitpid(pid, &status, WNOHANG) == pid) return stopping ? "interrupted" : "transcode_timeout";
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            kill(-pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return stopping ? "interrupted" : "transcode_timeout";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
}

MediaJobs::MediaJobs(asio::any_io_executor executor, const std::string& root)
    : timer_(executor), store_(root), work_root_(fs::absolute(root) / "jobs") {
    const char* program = std::getenv("CHAT_MEDIA_PROCESS");
    program_ = program ? program : "";
    const char* type = std::getenv("CHAT_HLS_SEGMENT_TYPE");
    segment_type_ = type ? type : "ts";
    if (segment_type_ != "ts" && segment_type_ != "fmp4") throw MediaError("invalid_segment_type");
    if (!program_.empty() && (!fs::path(program_).is_absolute() || access(program_.c_str(), X_OK)))
        throw MediaError("media_processor_unavailable");
}

MediaJobs::~MediaJobs() { stop(); worker_.join(); }
void MediaJobs::start() {
    if (program_.empty()) { std::cerr << "[MEDIA JOB] processor is not configured\n"; return; }
    fs::create_directories(work_root_);
    fs::permissions(work_root_, fs::perms::owner_all);
    asio::co_spawn(timer_.get_executor(), run(), asio::detached);
}
void MediaJobs::stop() {
    stopping_ = true;
    boost::system::error_code ignored;
    timer_.cancel(ignored);
}

asio::awaitable<void> MediaJobs::run() {
    // 文件锁保证同一存储目录只有一个处理器，重启恢复不会抢走其他进程的任务。
    const int lock = open((work_root_ / "worker_lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        if (lock >= 0) close(lock);
        std::cerr << "[MEDIA JOB] worker lock unavailable\n";
        co_return;
    }
    const FileLock release{lock};
    bool recovered = false;
    json completed;
    while (!stopping_) {
        bool found = false;
        try {
            json job;
            {
                auto guard = co_await AsyncConnectionGuard::create();
                if (!guard->valid()) throw MediaError("database_unavailable");
                MediaRepository repository(guard->connection());
                if (!recovered) { co_await repository.recover_jobs(); recovered = true; }
                // 数据库短暂故障时保留完成结果，重试登记，不把任务永远留在 processing。
                if (!completed.empty()) {
                    co_await repository.finish_job(completed["media_id"], completed["failure"]);
                    completed = json::object();
                    running_id_.clear();
                }
                job = co_await repository.claim_job();
            }
            if (!job.empty()) {
                found = true;
                running_id_ = job["media_id"];
                std::cout << "[MEDIA JOB] processing " << running_id_ << std::endl;
                const auto failure = co_await asio::co_spawn(worker_, Work<std::string>(
                    [this, job] { return process(job); }), asio::use_awaitable);
                if (stopping_) co_return;
                completed = {{"media_id", running_id_}, {"failure", failure}};
                std::cout << "[MEDIA JOB] " << (failure.empty() ? "ready " : failure + " ") << running_id_ << std::endl;
            }
        } catch (const std::exception& error) {
            std::cerr << "[MEDIA JOB] scheduling deferred: " << error.what() << std::endl;
        }
        if (found) continue;
        timer_.expires_after(std::chrono::seconds(1));
        boost::system::error_code error;
        co_await timer_.async_wait(asio::redirect_error(asio::use_awaitable, error));
    }
}

std::string MediaJobs::process(const json& job) {
    const std::string id = job.at("media_id"), hash = job.at("sha256");
    const auto work = store_.Path(id + "/processing");
    try {
        // 发布完成而状态提交前进程中断时，复用已验证的不可变版本。
        if (LoadHls(store_, id, hash)) return "";
        const auto source = store_.Path(id + "/original");
        if (Digest(source) != hash) throw MediaError("source_hash_mismatch");
        if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != id.npos)
            throw MediaError("invalid_media_id");
        fs::remove_all(work);
        fs::create_directory(work);
        fs::permissions(work, fs::perms::owner_all);
        const auto failure = Execute(program_, source.string(), (work / "bundle").string(),
                                     segment_type_, (work / "processor.log").string(), stopping_);
        if (!failure.empty()) {
            fs::remove_all(work / "bundle");
            fs::remove_all(work / "bundle.part");
            return failure;
        }
        if (stopping_) return "interrupted";
        publish(id, hash, work / "bundle");
        std::error_code cleanup_error;
        fs::remove_all(work, cleanup_error);
        return "";
    } catch (const std::exception& error) {
        std::error_code ignored;
        fs::remove_all(work / "bundle", ignored);
        fs::remove_all(work / "bundle.part", ignored);
        std::cerr << "[MEDIA JOB] output rejected " << id << ": " << error.what() << std::endl;
        return "publish_failed";
    }
}

void MediaJobs::publish(const std::string& id, const std::string& hash, const fs::path& folder) {
    const auto path = folder / "catalog.json";
    if (fs::file_size(path) > 512 * 1024) throw MediaError("invalid_catalog");
    std::ifstream input(path);
    const auto data = json::parse(input);
    const auto catalog = ParseHls(data, id, hash);
    try {
        for (const auto& item : data.at("assets")) {
            if (stopping_) throw MediaError("interrupted");
            const std::string name = item.at("path");
            const auto source = folder / name;
            const auto& asset = catalog->assets.at(name);
            if (fs::is_symlink(source) || fs::file_size(source) != asset.bytes ||
                Digest(source) != item.at("sha256").get<std::string>())
                throw MediaError("asset_hash_mismatch");
            auto output = store_.Begin(asset.key, asset.bytes, 256 * 1024 * 1024);
            std::ifstream file(source, std::ios::binary);
            std::array<std::uint8_t, 65536> bytes{};
            while (file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || file.gcount())
                output->Append(std::span(bytes.data(), static_cast<std::size_t>(file.gcount())));
            if (!file.eof()) throw MediaError("io_error");
            output->Commit();
        }
    } catch (...) {
        store_.RemoveTree(id + "/hls/" + catalog->revision);
        throw;
    }
    // 子资源均已落盘才公开 catalog。HTTP 凭证绑定此版本，读者看不到半成品。
    // 原子替换后 fsync 仍可能报错，因此不能再删除可能已被读者看到的版本。
    const auto text = data.dump();
    store_.Replace(id + "/hls/catalog", std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}
}
