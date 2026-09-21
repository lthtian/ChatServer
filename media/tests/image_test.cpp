// ABOUTME: Exercises image processing using actual JPEG and PNG data.
// ABOUTME: Checks display dimensions, transparency, orientation and input limits.
#include "chat/media/image.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

void Require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

std::vector<std::uint8_t> Encode(const char* extension, const cv::Mat& image) {
  std::vector<std::uint8_t> encoded;
  Require(cv::imencode(extension, image, encoded), "fixture encoding failed");
  return encoded;
}

void ExpectError(chat::media::ImageErrorCode code,
                 const std::function<void()>& operation) {
  try {
    operation();
  } catch (const chat::media::ImageError& error) {
    Require(error.code() == code, "unexpected image error code");
    return;
  }
  throw std::runtime_error("image was accepted unexpectedly");
}

void Test(const std::string& scenario) {
  using chat::media::CreateThumbnail;
  using chat::media::ImageErrorCode;
  using chat::media::ImageLimits;
  const cv::Mat source(600, 1200, CV_8UC3, cv::Scalar(40, 80, 160));
  if (scenario == "jpeg") {
    const auto encoded = Encode(".jpg", source);
    const auto result = CreateThumbnail(encoded);
    Require(result.source_width == 1200 && result.source_height == 600,
            "incorrect source dimensions");
    Require(result.width == 320 && result.height == 160,
            "incorrect thumbnail dimensions");
    Require(result.source_mime == "image/jpeg" && result.mime == "image/jpeg",
            "incorrect JPEG MIME type");
    const auto decoded = cv::imdecode(result.bytes, cv::IMREAD_UNCHANGED);
    Require(decoded.cols == 320 && decoded.rows == 160, "invalid output JPEG");
    const auto pixel = decoded.at<cv::Vec3b>(80, 160);
    Require(std::abs(pixel[2] - 160) < 5, "thumbnail color changed");
  } else if (scenario == "alpha") {
    const cv::Mat transparent(400, 800, CV_8UC4, cv::Scalar(10, 20, 30, 73));
    const auto result = CreateThumbnail(Encode(".png", transparent));
    const auto decoded = cv::imdecode(result.bytes, cv::IMREAD_UNCHANGED);
    Require(result.mime == "image/png" && decoded.channels() == 4,
            "alpha channel was discarded");
    Require(decoded.cols == 320 && decoded.rows == 160, "incorrect PNG size");
    Require(decoded.at<cv::Vec4b>(80, 160)[3] == 73, "alpha value changed");
  } else if (scenario == "small") {
    const cv::Mat small(13, 21, CV_8UC3, cv::Scalar(0, 0, 0));
    const auto result = CreateThumbnail(Encode(".png", small));
    Require(result.width == 21 && result.height == 13, "small image upscaled");
  } else if (scenario == "orientation") {
    auto encoded = Encode(".jpg", source);
    const std::vector<std::uint8_t> exif = {
        0xff, 0xe1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
        'I', 'I', 0x2a, 0, 8, 0, 0, 0, 1, 0,
        0x12, 1, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0};
    encoded.insert(encoded.begin() + 2, exif.begin(), exif.end());
    const auto result = CreateThumbnail(encoded);
    Require(result.source_width == 600 && result.source_height == 1200,
            "EXIF orientation was not applied");
    Require(result.width == 160 && result.height == 320,
            "oriented thumbnail dimensions are incorrect");
  } else if (scenario == "corrupt") {
    const std::vector<std::uint8_t> truncated = {0xff, 0xd8, 0xff, 0xe0, 0, 16};
    ExpectError(ImageErrorCode::kInvalidImage,
                [&] { CreateThumbnail(truncated); });
  } else if (scenario == "byte_limit") {
    const auto encoded = Encode(".jpg", source);
    ImageLimits limits;
    limits.max_bytes = encoded.size() - 1;
    ExpectError(ImageErrorCode::kByteLimit,
                [&] { CreateThumbnail(encoded, limits); });
  } else if (scenario == "pixel_limit") {
    const auto encoded = Encode(".png", source);
    ImageLimits limits;
    limits.max_pixels = 1200 * 600 - 1;
    ExpectError(ImageErrorCode::kPixelLimit,
                [&] { CreateThumbnail(encoded, limits); });
    auto oversized = encoded;
    oversized[16] = 0x7f;
    ExpectError(ImageErrorCode::kPixelLimit,
                [&] { CreateThumbnail(oversized); });
  } else if (scenario == "format") {
    const auto encoded = Encode(".bmp", source);
    ExpectError(ImageErrorCode::kUnsupportedFormat,
                [&] { CreateThumbnail(encoded); });
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
