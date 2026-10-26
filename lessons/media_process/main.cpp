#include "media_io.h"

#include "hls_package.h"

#include <chrono>
#include <charconv>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <set>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
#ifdef _WIN32
// Windows 参数是 UTF-16，FFmpeg 与程序内部统一使用 UTF-8。
std::string Argument(const wchar_t* value) {
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                       nullptr, 0, nullptr, nullptr);
  if (size == 0) throw std::runtime_error("invalid command-line text");
  std::string result(size, '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                          result.data(), size, nullptr, nullptr) != size)
    throw std::runtime_error("invalid command-line text");
  result.pop_back();
  return result;
}
#else
std::string Argument(const char* value) { return value; }
#endif
}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[])
#else
int main(int argc, char* argv[])
#endif
{
  try {
    if (argc < 4) {
      std::cerr << "usage: media_process remux input output.mp4|output.mkv|output.ts\n"
                << "       media_process transcode input output-directory [--gop-seconds 1..10] [--b-frames 0..8] [--crf 1..51] [--max-rate-kbps 64..20000]\n"
                << "       media_process hls input output-directory [ts|fmp4]\n";
      return 2;
    }
    av_log_set_level(AV_LOG_WARNING);
    const auto started = std::chrono::steady_clock::now();
    const std::string mode = Argument(argv[1]);
    if (mode == "remux") {
      if (argc != 4) throw std::runtime_error("remux accepts input and output only");
      media::Remux(Argument(argv[2]), Argument(argv[3]));
    } else if (mode == "transcode") {
      media::TranscodeOptions options;
      std::set<std::string> seen;
      for (int i = 4; i < argc; i += 2) {
        if (i + 1 == argc) throw std::runtime_error("transcode option needs an integer value");
        const std::string name = Argument(argv[i]);
        const std::string value = Argument(argv[i + 1]);
        if (!seen.insert(name).second) throw std::runtime_error("duplicate transcode option: " + name);
        int number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
          throw std::runtime_error("transcode option needs an integer value: " + name);
        if (name == "--gop-seconds") options.gop_seconds = number;
        else if (name == "--b-frames") options.b_frames = number;
        else if (name == "--crf") options.crf = number;
        else if (name == "--max-rate-kbps") {
          if (number < 64 || number > 20000) throw std::runtime_error("max-rate-kbps must be 64..20000");
          options.max_video_rate = number * 1000;
        } else throw std::runtime_error("unknown transcode option: " + name);
      }
      media::TranscodeRenditions(Argument(argv[2]), Argument(argv[3]), options);
    } else if (mode == "hls") {
      // HLS 档位与两秒 IDR 对齐是打包契约，不接受普通转码实验的自定义 GOP。
      if (argc > 5 || (argc == 5 && Argument(argv[4]) != "ts" && Argument(argv[4]) != "fmp4"))
        throw std::runtime_error("hls accepts only an optional segment type: ts or fmp4");
      media::PackageHls(Argument(argv[2]), Argument(argv[3]), argc == 5 && Argument(argv[4]) == "fmp4");
    }
    else throw std::runtime_error("mode must be remux, transcode or hls");
    std::cout << "completed elapsed_s=" << std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
