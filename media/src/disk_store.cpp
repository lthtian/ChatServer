// ABOUTME: Stores immutable media objects beneath a private filesystem root.
// ABOUTME: Publishes complete uploads without overwriting existing object keys.
#include "chat/media/disk_store.h"

#include <cerrno>
#include <random>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace chat::media {
namespace {
namespace fs = std::filesystem;

[[noreturn]] void FileError(const char* operation) {
  throw std::system_error(errno ? errno : EIO, std::generic_category(), operation);
}

void CheckLinks(const fs::path& path) {
  fs::path current = path.root_path();
  for (const auto& part : path.relative_path()) {
    current /= part;
    std::error_code error;
    const auto status = fs::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory) return;
    if (error) throw std::system_error(error, "Inspect storage path");
    if (fs::is_symlink(status)) {
      throw std::invalid_argument("Storage paths cannot contain symbolic links");
    }
  }
}

void CreateDirectories(const fs::path& path) {
  CheckLinks(path);
  fs::path current = path.root_path();
  for (const auto& part : path.relative_path()) {
    current /= part;
    if (fs::create_directory(current)) {
      fs::permissions(current, fs::perms::owner_all);
    }
  }
  CheckLinks(path);
}

void SyncDirectory(const fs::path& path) {
#ifndef _WIN32
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (descriptor < 0) FileError("Open object directory");
  const int result = ::fsync(descriptor);
  const int saved_errno = errno;
  ::close(descriptor);
  if (result != 0) {
    throw std::system_error(saved_errno, std::generic_category(),
                            "Synchronize object directory");
  }
#else
  static_cast<void>(path);
#endif
}

void ValidateKey(std::string_view key) {
  if (key.empty() || key.size() > 240 || key.front() == '/' || key.back() == '/') {
    throw std::invalid_argument("Invalid object key");
  }
  bool slash = false;
  for (const auto character : key) {
    const bool allowed = (character >= 'a' && character <= 'z') ||
                         (character >= '0' && character <= '9') ||
                         character == '-' || character == '_' || character == '/';
    if (!allowed || (character == '/' && slash)) {
      throw std::invalid_argument("Invalid object key");
    }
    slash = character == '/';
  }
}

}  // namespace

DiskStore::DiskStore(const fs::path& root)
    : root_(fs::absolute(root).lexically_normal()) {
  if (root.empty() || root_ == root_.root_path()) {
    throw std::invalid_argument("A dedicated storage directory is required");
  }
  CreateDirectories(root_ / "objects");
  CreateDirectories(root_ / "staging");
}

fs::path DiskStore::ObjectPath(std::string_view key) const {
  ValidateKey(key);
  const auto path = root_ / "objects" / std::string(key);
  CheckLinks(path);
  return path;
}

std::unique_ptr<DiskUpload> DiskStore::Begin(std::string_view key,
                                            std::uint64_t expected_bytes,
                                            std::uint64_t max_bytes) const {
  const auto destination = ObjectPath(key);
  if (expected_bytes > max_bytes) {
    throw std::invalid_argument("Invalid upload size");
  }
  CreateDirectories(destination.parent_path());
  CheckLinks(root_ / "staging");
  std::random_device random;
  for (int attempt = 0; attempt < 20; ++attempt) {
    const auto staging = root_ / "staging" /
        (std::to_string(random()) + "-" + std::to_string(random()));
    if (fs::create_directory(staging)) {
      try {
        fs::permissions(staging, fs::perms::owner_all);
        return std::unique_ptr<DiskUpload>(
            new DiskUpload(staging, destination, expected_bytes));
      } catch (...) {
        std::error_code ignored;
        fs::remove(staging, ignored);
        throw;
      }
    }
  }
  throw std::runtime_error("Cannot reserve upload staging directory");
}

std::ifstream DiskStore::Open(std::string_view key) const {
  const auto path = ObjectPath(key);
  if (!fs::is_regular_file(path)) {
    throw std::system_error(std::make_error_code(std::errc::no_such_file_or_directory),
                            "Object is not a regular file");
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) FileError("Open object");
  return stream;
}

std::uint64_t DiskStore::Size(std::string_view key) const {
  return fs::file_size(ObjectPath(key));
}

bool DiskStore::Remove(std::string_view key) const {
  const auto path = ObjectPath(key);
  if (fs::is_directory(path)) throw std::invalid_argument("Object key is a directory");
  const bool removed = fs::remove(path);
  if (removed) SyncDirectory(path.parent_path());
  return removed;
}

void DiskStore::CollectStaging() const {
  const auto directory = root_ / "staging";
  CheckLinks(directory);
  const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24);
  for (const auto& entry : fs::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (name.empty() || name.find_first_not_of("0123456789-") != std::string::npos ||
        entry.is_symlink() || !entry.is_directory() || entry.last_write_time() >= cutoff) continue;
    CheckLinks(entry.path() / "body");
    fs::remove(entry.path() / "body");
    fs::remove(entry.path());
  }
}

DiskUpload::DiskUpload(fs::path staging, fs::path destination,
                       std::uint64_t expected_bytes)
    : staging_(std::move(staging)), destination_(std::move(destination)),
      expected_bytes_(expected_bytes) {
  const auto path = staging_ / "body";
#ifdef _WIN32
  const auto error = ::_wfopen_s(&file_, path.c_str(), L"wb");
  if (error != 0) {
    throw std::system_error(error, std::generic_category(), "Create staged object");
  }
#else
  file_ = std::fopen(path.c_str(), "wb");
#endif
  if (!file_) FileError("Create staged object");
}

DiskUpload::~DiskUpload() { Cleanup(); }

void DiskUpload::Cleanup() noexcept {
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
  }
  std::error_code ignored;
  fs::remove(staging_ / "body", ignored);
  fs::remove(staging_, ignored);
}

void DiskUpload::Append(std::span<const std::uint8_t> bytes) {
  if (!file_ || committed_) throw std::logic_error("Upload is closed");
  if (bytes.size() > expected_bytes_ - received_bytes_) {
    Cleanup();
    throw std::length_error("Upload exceeds declared size");
  }
  if (std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) {
    const int saved_errno = errno;
    Cleanup();
    throw std::system_error(saved_errno ? saved_errno : EIO,
                            std::generic_category(), "Write staged object");
  }
  received_bytes_ += bytes.size();
}

void DiskUpload::Commit() {
  if (!file_ || committed_) throw std::logic_error("Upload is closed");
  if (received_bytes_ != expected_bytes_) {
    Cleanup();
    throw std::runtime_error("Upload is incomplete");
  }
  try {
    if (std::fflush(file_) != 0) FileError("Flush staged object");
#ifdef _WIN32
    if (::_commit(::_fileno(file_)) != 0) FileError("Synchronize staged object");
#else
    if (::fsync(::fileno(file_)) != 0) FileError("Synchronize staged object");
#endif
    auto* closing = file_;
    file_ = nullptr;
    if (std::fclose(closing) != 0) FileError("Close staged object");
    CheckLinks(destination_);
    // A hard link atomically publishes without replacing another writer's object.
    fs::create_hard_link(staging_ / "body", destination_);
    committed_ = true;
    SyncDirectory(destination_.parent_path());
    Cleanup();
  } catch (...) {
    Cleanup();
    throw;
  }
}

}  // namespace chat::media
