#include "media_io.h"

#include "hls_package.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
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
    if (argc != 4 && argc != 5) {
      std::cerr << "usage: media_process remux input output.mp4|output.mkv\n"
                << "       media_process transcode input output-directory\n"
                << "       media_process hls input output-directory [ts|fmp4]\n";
      return 2;
    }
    av_log_set_level(AV_LOG_WARNING);
    const auto started = std::chrono::steady_clock::now();
    const std::string mode = Argument(argv[1]);
    if (argc == 5 && (mode != "hls" || (Argument(argv[4]) != "ts" && Argument(argv[4]) != "fmp4")))
      throw std::runtime_error("only hls accepts a fourth argument: ts or fmp4");
    if (mode == "remux") media::Remux(Argument(argv[2]), Argument(argv[3]));
    else if (mode == "transcode") media::TranscodeRenditions(Argument(argv[2]), Argument(argv[3]));
    else if (mode == "hls") media::PackageHls(Argument(argv[2]), Argument(argv[3]), argc == 5 && Argument(argv[4]) == "fmp4");
    else throw std::runtime_error("mode must be remux, transcode or hls");
    std::cout << "completed elapsed_s=" << std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
