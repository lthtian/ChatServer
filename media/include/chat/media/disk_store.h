// ABOUTME: Defines private disk objects with bounded writes and immutable publication.
// ABOUTME: Provides staging ownership so abandoned uploads are removed automatically.
#ifndef CHAT_MEDIA_DISK_STORE_H_
#define CHAT_MEDIA_DISK_STORE_H_

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string_view>

namespace chat::media {

class DiskUpload {
 public:
  ~DiskUpload();
  DiskUpload(const DiskUpload&) = delete;
  DiskUpload& operator=(const DiskUpload&) = delete;
  void Append(std::span<const std::uint8_t> bytes);
  void Commit();
  std::uint64_t received_bytes() const noexcept { return received_bytes_; }

 private:
  friend class DiskStore;
  DiskUpload(std::filesystem::path staging, std::filesystem::path destination,
             std::uint64_t expected_bytes);
  void Cleanup() noexcept;

  std::filesystem::path staging_;
  std::filesystem::path destination_;
  std::FILE* file_ = nullptr;
  std::uint64_t expected_bytes_ = 0;
  std::uint64_t received_bytes_ = 0;
  bool committed_ = false;
};

class DiskStore {
 public:
  explicit DiskStore(const std::filesystem::path& root);
  std::unique_ptr<DiskUpload> Begin(std::string_view key,
                                   std::uint64_t expected_bytes,
                                   std::uint64_t max_bytes) const;
  std::ifstream Open(std::string_view key) const;
  std::uint64_t Size(std::string_view key) const;
  bool Remove(std::string_view key) const;
  void CollectStaging() const;

 private:
  std::filesystem::path ObjectPath(std::string_view key) const;
  std::filesystem::path root_;
};

}  // namespace chat::media

#endif  // CHAT_MEDIA_DISK_STORE_H_
