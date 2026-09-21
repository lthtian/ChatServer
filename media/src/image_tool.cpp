// ABOUTME: Produces a thumbnail and JSON metadata for a local image file.
// ABOUTME: Supplies a process boundary for desktop media processing tasks.
#include "chat/media/disk_store.h"
#include "chat/media/image.h"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int Fail(const char* code) {
  std::cout << nlohmann::json{{"error", code}}.dump() << '\n';
  return 1;
}

template <typename Character>
int Run(int argc, Character** argv) {
  try {
    if (argc != 4) return Fail("invalid_argument");
    const std::filesystem::path source(argv[1]);
    const std::filesystem::path destination(argv[2]);
    std::string key;
    for (auto* character = argv[3]; *character; ++character) {
      if (*character < 0 || *character > 127) return Fail("invalid_argument");
      key.push_back(static_cast<char>(*character));
    }
    if (!std::filesystem::is_regular_file(source)) return Fail("io_error");
    const auto bytes = std::filesystem::file_size(source);
    const chat::media::ImageLimits limits;
    if (bytes > limits.max_bytes) return Fail("byte_limit");
    std::ifstream input(source, std::ios::binary);
    if (!input) return Fail("io_error");
    std::vector<std::uint8_t> encoded(static_cast<std::size_t>(bytes));
    if (!input.read(reinterpret_cast<char*>(encoded.data()),
                    static_cast<std::streamsize>(encoded.size())) ||
        input.peek() != std::char_traits<char>::eof()) return Fail("io_error");
    const auto thumbnail = chat::media::CreateThumbnail(encoded, limits);
    chat::media::DiskStore store(destination);
    auto upload = store.Begin(key, thumbnail.bytes.size(), limits.max_bytes);
    upload->Append(thumbnail.bytes);
    upload->Commit();
    const nlohmann::json result = {
        {"source", {{"width", thumbnail.source_width},
                    {"height", thumbnail.source_height},
                    {"mime", thumbnail.source_mime}}},
        {"thumbnail", {{"key", key}, {"width", thumbnail.width},
                       {"height", thumbnail.height}, {"mime", thumbnail.mime},
                       {"bytes", thumbnail.bytes.size()}}}};
    std::cout << result.dump() << '\n';
    return 0;
  } catch (const chat::media::ImageError& error) {
    using chat::media::ImageErrorCode;
    switch (error.code()) {
      case ImageErrorCode::kInvalidImage: return Fail("invalid_image");
      case ImageErrorCode::kUnsupportedFormat: return Fail("unsupported_format");
      case ImageErrorCode::kByteLimit: return Fail("byte_limit");
      case ImageErrorCode::kPixelLimit: return Fail("pixel_limit");
    }
  } catch (const std::invalid_argument&) {
    return Fail("invalid_argument");
  } catch (const std::exception&) {
    return Fail("io_error");
  }
  return Fail("io_error");
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return Run(argc, argv); }
#else
int main(int argc, char** argv) { return Run(argc, argv); }
#endif
