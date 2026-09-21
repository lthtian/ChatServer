// ABOUTME: Defines bounded static image processing and thumbnail results.
// ABOUTME: Keeps image processing independent of networking and database state.
#ifndef CHAT_MEDIA_IMAGE_H_
#define CHAT_MEDIA_IMAGE_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace chat::media {

struct ImageLimits {
  std::size_t max_bytes = 20 * 1024 * 1024;
  std::uint64_t max_pixels = 24000000;
  int max_dimension = 16000;
  int thumbnail_edge = 320;
};

enum class ImageErrorCode {
  kInvalidImage,
  kUnsupportedFormat,
  kByteLimit,
  kPixelLimit,
};

class ImageError : public std::runtime_error {
 public:
  ImageError(ImageErrorCode code, const std::string& message)
      : std::runtime_error(message), code_(code) {}
  ImageErrorCode code() const noexcept { return code_; }

 private:
  ImageErrorCode code_;
};

struct Thumbnail {
  std::string source_mime;
  int source_width = 0;
  int source_height = 0;
  std::string mime;
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> bytes;
};

Thumbnail CreateThumbnail(std::span<const std::uint8_t> encoded,
                          const ImageLimits& limits = {});

}  // namespace chat::media

#endif  // CHAT_MEDIA_IMAGE_H_
