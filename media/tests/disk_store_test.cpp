// ABOUTME: Tests disk publication and failures against real temporary files.
// ABOUTME: Verifies upload isolation, limits, immutable objects and traversal rejection.
// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/09/16 16:16
#include "chat/media/disk_store.h"

#include <array>
#include <functional>
#include <iostream>
#include <iterator>
#include <random>
#include <string>

namespace {

void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::random_device random;
    for (int attempt = 0; attempt < 20; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("chat-media-test-" + std::to_string(random()));
      if (std::filesystem::create_directory(path_)) return;
    }
    throw std::runtime_error("cannot create temporary directory");
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

template <typename Error>
void ExpectError(const std::function<void()>& operation) {
  try {
    operation();
  } catch (const Error&) {
    return;
  }
  throw std::runtime_error("operation was accepted unexpectedly");
}

std::string Read(const chat::media::DiskStore& store, std::string_view key) {
  auto stream = store.Open(key);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void Test(const std::string& scenario) {
  TemporaryDirectory temporary;
  chat::media::DiskStore store(temporary.path());
  const std::array<std::uint8_t, 3> bytes{'a', 'b', 'c'};
  const auto staged_files = [&] {
    return std::distance(std::filesystem::directory_iterator(temporary.path() / "staging"),
                         std::filesystem::directory_iterator());
  };
  if (scenario == "publish") {
    auto upload = store.Begin("images/abc/original", 3, 20);
    upload->Append(bytes);
    Require(upload->received_bytes() == 3, "incorrect upload byte count");
    ExpectError<std::system_error>([&] { store.Open("images/abc/original"); });
    upload->Commit();
    Require(Read(store, "images/abc/original") == "abc", "object bytes changed");
    Require(store.Size("images/abc/original") == 3, "incorrect object size");
    Require(staged_files() == 0, "committed staging file leaked");
    ExpectError<std::logic_error>([&] { upload->Append(bytes); });
    Require(store.Remove("images/abc/original"), "object was not removed");
    Require(!store.Remove("images/abc/original"), "remove is not idempotent");
  } else if (scenario == "abandon") {
    {
      auto upload = store.Begin("images/abc/original", 6, 20);
      upload->Append(bytes);
      ExpectError<std::runtime_error>([&] { upload->Commit(); });
    }
    Require(staged_files() == 0, "abandoned staging file leaked");
    ExpectError<std::system_error>([&] { store.Open("images/abc/original"); });
  } else if (scenario == "limit") {
    ExpectError<std::invalid_argument>([&] { store.Begin("images/abc", 21, 20); });
    {
      auto upload = store.Begin("images/abc", 2, 20);
      ExpectError<std::length_error>([&] { upload->Append(bytes); });
      ExpectError<std::logic_error>([&] { upload->Commit(); });
    }
    Require(staged_files() == 0, "oversized upload staging file leaked");
  } else if (scenario == "immutable") {
    auto first = store.Begin("images/abc", 3, 20);
    auto second = store.Begin("images/abc", 3, 20);
    first->Append(bytes);
    const std::array<std::uint8_t, 3> alternate{'x', 'y', 'z'};
    second->Append(alternate);
    first->Commit();
    ExpectError<std::system_error>([&] { second->Commit(); });
    second.reset();
    Require(Read(store, "images/abc") == "abc", "existing object was overwritten");
    Require(staged_files() == 0, "duplicate upload staging file leaked");
  } else if (scenario == "key") {
    for (const auto* key : {"", "../escape", "a/../b", "/absolute", "C:/absolute",
                            "a\\b", "a//b", "a/", "a/./b", "a/b.txt", "CON"}) {
      ExpectError<std::invalid_argument>([&] { store.Begin(key, 3, 20); });
      ExpectError<std::invalid_argument>([&] { store.Open(key); });
    }
    const auto abandoned = temporary.path() / "staging" / "123-456";
    std::filesystem::create_directory(abandoned);
    std::ofstream(abandoned / "body") << "partial";
    std::filesystem::last_write_time(abandoned,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(25));
    auto active = store.Begin("active/image", 3, 20);
    store.CollectStaging();
    Require(!std::filesystem::exists(abandoned), "expired staging file leaked");
    Require(staged_files() == 1, "active staging file was collected");
  } else {
    throw std::runtime_error("unknown test scenario");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Require(argc == 2, "one scenario argument is required");
    Test(argv[1]);
    std::cout << "PASS " << argv[1] << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
