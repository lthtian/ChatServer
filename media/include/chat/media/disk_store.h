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
  // 将小型状态/清单完整落盘后原子替换；媒体原件仍使用 Begin 的不可覆盖提交。
  void Replace(std::string_view key, std::span<const std::uint8_t> bytes) const;
  // 供受控媒体处理子进程读取对象；仍校验对象键和符号链接。
  std::filesystem::path Path(std::string_view key) const { return ObjectPath(key); }
  // 删除指定媒体的派生目录，拒绝链接与非目录；调用者先确认任务不在运行。
  void RemoveTree(std::string_view key) const;

 private:
  std::filesystem::path ObjectPath(std::string_view key) const;
  std::filesystem::path root_;
};

}  // namespace chat::media

#endif  // CHAT_MEDIA_DISK_STORE_H_
