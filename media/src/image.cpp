// ABOUTME: Validates encoded images and produces bounded thumbnails.
// ABOUTME: Enforces media limits before allocating decoded image pixels.
// copyright 2026 The Master Lu PC-Group Authors. All rights reserved.
// author  jiadebin@ludashi.com
// date 2026/09/16 16:16
#include "chat/media/image.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace chat::media {
namespace {

struct Header {
  bool png;
  std::uint32_t width;
  std::uint32_t height;
};

[[noreturn]] void Invalid() {
  throw ImageError(ImageErrorCode::kInvalidImage, "Invalid or incomplete image");
}

std::uint32_t BigEndian(std::span<const std::uint8_t> bytes) {
  std::uint32_t value = 0;
  for (auto byte : bytes) value = (value << 8) | byte;
  return value;
}

void CheckDimensions(const Header& header, const ImageLimits& limits) {
  if (header.width == 0 || header.height == 0) Invalid();
  if (header.width > static_cast<std::uint32_t>(limits.max_dimension) ||
      header.height > static_cast<std::uint32_t>(limits.max_dimension) ||
      std::uint64_t{header.width} * header.height > limits.max_pixels) {
    throw ImageError(ImageErrorCode::kPixelLimit, "Image dimensions exceed limit");
  }
}

Header Inspect(std::span<const std::uint8_t> bytes, const ImageLimits& limits) {
  constexpr std::array<std::uint8_t, 8> png_signature = {
      137, 80, 78, 71, 13, 10, 26, 10};
  if (bytes.size() >= 8 &&
      std::equal(png_signature.begin(), png_signature.end(), bytes.begin())) {
    if (bytes.size() < 33 || BigEndian(bytes.subspan(8, 4)) != 13 ||
        BigEndian(bytes.subspan(12, 4)) != 0x49484452) Invalid();
    Header header{true, BigEndian(bytes.subspan(16, 4)),
                  BigEndian(bytes.subspan(20, 4))};
    CheckDimensions(header, limits);
    bool image_data = false;
    for (std::size_t offset = 8; offset <= bytes.size() - 12;) {
      const auto length = BigEndian(bytes.subspan(offset, 4));
      if (length > bytes.size() - offset - 12) Invalid();
      const auto type = BigEndian(bytes.subspan(offset + 4, 4));
      if (type == 0x6163544c) {
        throw ImageError(ImageErrorCode::kUnsupportedFormat,
                         "Animated PNG is not supported");
      }
      if (type == 0x49484452 && offset != 8) Invalid();
      if (type == 0x49444154) image_data = true;
      offset += static_cast<std::size_t>(length) + 12;
      if (type == 0x49454e44) {
        if (length != 0 || !image_data || offset != bytes.size()) Invalid();
        return header;
      }
    }
    Invalid();
  }
  if (bytes.size() < 2 || bytes[0] != 0xff || bytes[1] != 0xd8) {
    throw ImageError(ImageErrorCode::kUnsupportedFormat,
                     "Only static JPEG and PNG images are supported");
  }
  Header header{false, 0, 0};
  bool frame_found = false;
  std::size_t offset = 2;
  while (offset < bytes.size()) {
    if (bytes[offset++] != 0xff) Invalid();
    while (offset < bytes.size() && bytes[offset] == 0xff) ++offset;
    if (offset >= bytes.size()) Invalid();
    const auto marker = bytes[offset++];
    if (marker == 0 || marker == 0xd8 || marker == 0xd9 || marker == 1 ||
        (marker >= 0xd0 && marker <= 0xd7)) Invalid();
    if (bytes.size() - offset < 2) Invalid();
    const auto length = BigEndian(bytes.subspan(offset, 2));
    if (length < 2 || length > bytes.size() - offset) Invalid();
    const bool frame = marker >= 0xc0 && marker <= 0xcf &&
                       marker != 0xc4 && marker != 0xc8 && marker != 0xcc;
    if (frame) {
      if (marker > 0xc2) {
        throw ImageError(ImageErrorCode::kUnsupportedFormat,
                         "JPEG encoding is not supported");
      }
      if (frame_found || length < 8 || bytes[offset + 2] != 8 ||
          length != 8u + 3u * bytes[offset + 7]) Invalid();
      header.height = BigEndian(bytes.subspan(offset + 3, 2));
      header.width = BigEndian(bytes.subspan(offset + 5, 2));
      CheckDimensions(header, limits);
      frame_found = true;
    }
    if (marker == 0xda) {
      if (!frame_found || length < 6 || offset + length >= bytes.size() - 2 ||
          bytes[bytes.size() - 2] != 0xff || bytes.back() != 0xd9) Invalid();
      return header;
    }
    offset += length;
  }
  Invalid();
}

}  // namespace

Thumbnail CreateThumbnail(std::span<const std::uint8_t> encoded,
                          const ImageLimits& limits) {
  if (limits.max_bytes == 0 || limits.max_pixels == 0 ||
      limits.max_dimension <= 0 || limits.thumbnail_edge <= 0 ||
      limits.thumbnail_edge > limits.max_dimension) {
    throw std::invalid_argument("Invalid image limits");
  }
  if (encoded.size() > limits.max_bytes ||
      encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw ImageError(ImageErrorCode::kByteLimit, "Image bytes exceed limit");
  }
  const auto header = Inspect(encoded, limits);
  try {
    const cv::Mat input(1, static_cast<int>(encoded.size()), CV_8UC1,
                        const_cast<std::uint8_t*>(encoded.data()));
    // JPEG color decoding applies EXIF orientation; PNG preserves alpha.
    cv::Mat pixels = cv::imdecode(input, header.png ? cv::IMREAD_UNCHANGED
                                                   : cv::IMREAD_COLOR);
    if (pixels.empty()) Invalid();
    const bool matches = pixels.cols == static_cast<int>(header.width) &&
                         pixels.rows == static_cast<int>(header.height);
    const bool rotated = !header.png &&
                         pixels.cols == static_cast<int>(header.height) &&
                         pixels.rows == static_cast<int>(header.width);
    if (!matches && !rotated) Invalid();
    Thumbnail result;
    result.source_mime = header.png ? "image/png" : "image/jpeg";
    result.source_width = pixels.cols;
    result.source_height = pixels.rows;
    const double scale = std::min(1.0, static_cast<double>(limits.thumbnail_edge) /
                                         std::max(pixels.cols, pixels.rows));
    result.width = std::max(1, static_cast<int>(std::lround(pixels.cols * scale)));
    result.height = std::max(1, static_cast<int>(std::lround(pixels.rows * scale)));
    cv::Mat thumbnail;
    cv::resize(pixels, thumbnail, cv::Size(result.width, result.height), 0, 0,
               cv::INTER_AREA);
    if (thumbnail.depth() == CV_16U) {
      thumbnail.convertTo(thumbnail, CV_8U, 1.0 / 257.0);
    }
    const bool alpha = thumbnail.channels() == 4;
    result.mime = alpha ? "image/png" : "image/jpeg";
    const std::vector<int> parameters = alpha
        ? std::vector<int>{cv::IMWRITE_PNG_COMPRESSION, 6}
        : std::vector<int>{cv::IMWRITE_JPEG_QUALITY, 85};
    if (!cv::imencode(alpha ? ".png" : ".jpg", thumbnail, result.bytes,
                      parameters)) Invalid();
    return result;
  } catch (const cv::Exception&) {
    Invalid();
  }
}

}  // namespace chat::media
