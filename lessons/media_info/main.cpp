
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#endif

// FFmpeg 提供 C 接口；extern "C" 使 C++ 按 C 的函数符号进行链接。
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace {

// FFmpeg 用负数返回错误；转换后的文字说明具体失败原因。
void CheckResult(int result, const char* operation) {
  if (result >= 0) {
    return;
  }
  char message[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(result, message, sizeof(message));
  throw std::runtime_error(std::string(operation) + ": " + message);
}

std::string Timestamp(int64_t value) {
  return value == AV_NOPTS_VALUE ? "unknown" : std::to_string(value);
}

// time_base 表示一格时间戳的秒数，负时间戳也按相同公式换算。
std::string Seconds(int64_t value, AVRational time_base) {
  if (value == AV_NOPTS_VALUE) {
    return "unknown";
  }
  std::ostringstream output;
  output << std::fixed << std::setprecision(6) << value * av_q2d(time_base);
  return output.str();
}

using FramePtr = std::unique_ptr<AVFrame, void (*)(AVFrame*)>;

FramePtr CreateFrame() {
  FramePtr frame(av_frame_alloc(),
                 [](AVFrame* value) { av_frame_free(&value); });
  if (!frame) {
    throw std::bad_alloc();
  }
  return frame;
}

// 排他创建图片文件，已有文件（包括输入视频）不会被覆盖。
std::unique_ptr<FILE, decltype(&std::fclose)> CreateImageFile(
    const std::string& filename) {
  FILE* file = nullptr;
#ifdef _WIN32
  // Windows 文件接口使用 UTF-16；命令行路径已统一转换为 UTF-8。
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       filename.c_str(), -1, nullptr, 0);
  if (size == 0) {
    throw std::runtime_error("invalid output path");
  }
  std::wstring path(size, L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filename.c_str(), -1,
                          path.data(), size) != size) {
    throw std::runtime_error("invalid output path");
  }
  const int descriptor =
      _wopen(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
             _S_IREAD | _S_IWRITE);
  if (descriptor >= 0) {
    file = _fdopen(descriptor, "wb");
    if (!file) {
      _close(descriptor);
      throw std::runtime_error("cannot create image file");
    }
  }
#else
  file = std::fopen(filename.c_str(), "wbx");
#endif
  if (!file) {
    throw std::runtime_error(errno == EEXIST ? "output file already exists"
                                             : "cannot create image file");
  }
  return {file, &std::fclose};
}

void SaveFirstFrame(const AVFrame* source, const std::string& filename) {
  FramePtr rgb = CreateFrame();
  rgb->format = AV_PIX_FMT_RGB24;
  rgb->width = source->width;
  rgb->height = source->height;
  rgb->color_range = AVCOL_RANGE_JPEG;
  rgb->colorspace = AVCOL_SPC_RGB;
  rgb->color_primaries = source->color_primaries;
  rgb->color_trc = source->color_trc;

  const auto free_scaler = [](SwsContext* scaler) {
    sws_free_context(&scaler);
  };
  std::unique_ptr<SwsContext, decltype(free_scaler)> scaler(sws_alloc_context(),
                                                            free_scaler);
  if (!scaler) {
    throw std::bad_alloc();
  }
  scaler->flags = SWS_BILINEAR;
  scaler->threads = 1;
  // 帧接口读取源帧的格式、颜色参数和行跨度，并分配目标像素缓冲。
  // RGB24 每个像素占 3 字节，R/G/B 依次排列在同一个平面。
  CheckResult(sws_scale_frame(scaler.get(), rgb.get(), source),
              "sws_scale_frame");

  auto file = CreateImageFile(filename);
  // PPM P6 的三行文本头之后直接存 RGB 字节，不需要另建图片编码器。
  if (std::fprintf(file.get(), "P6\n%d %d\n255\n", rgb->width, rgb->height) <
      0) {
    throw std::runtime_error("cannot write image file");
  }
  const size_t row_bytes = static_cast<size_t>(rgb->width) * 3;
  for (int row = 0; row < rgb->height; ++row) {
    // linesize 是内存中相邻两行的跨度，可能比有效像素字节数更大。
    const uint8_t* pixels =
        rgb->data[0] + static_cast<std::ptrdiff_t>(row) * rgb->linesize[0];
    if (std::fwrite(pixels, 1, row_bytes, file.get()) != row_bytes) {
      throw std::runtime_error("cannot write image file");
    }
  }
  if (std::fclose(file.release()) != 0) {
    throw std::runtime_error("cannot write image file");
  }
  std::cout << "image format=ppm width=" << rgb->width
            << " height=" << rgb->height << " bytes=" << row_bytes * rgb->height
            << '\n';
}

// 一个对象持续处理选中视频流的包，参考图像等状态保存在 context_ 中。
class VideoDecoder {
 public:
  VideoDecoder(const AVStream* stream, const std::string& image_path)
      : stream_(stream),
        image_path_(image_path),
        context_(nullptr,
                 [](AVCodecContext* value) { avcodec_free_context(&value); }),
        frame_(CreateFrame()) {
    // AVCodec 是库管理的实现描述；AVCodecContext 是本次解码的工作实例。
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
      throw std::runtime_error("video decoder not found");
    }
    context_.reset(avcodec_alloc_context3(codec));
    if (!context_) {
      throw std::bad_alloc();
    }
    CheckResult(avcodec_parameters_to_context(context_.get(), stream->codecpar),
                "avcodec_parameters_to_context");
    context_->pkt_timebase = stream->time_base;
    // 单线程便于观察编码依赖带来的输出延迟，不引入帧线程的额外延迟。
    context_->thread_count = 1;
    CheckResult(avcodec_open2(context_.get(), codec, nullptr), "avcodec_open2");
    std::cout << "decoder stream=" << stream_->index << " codec=" << codec->name
              << '\n';
  }

  void Send(const AVPacket* packet) {
    const bool draining = packet == nullptr;
    // send 返回成功才表示接受了这个包；EAGAIN 时包还需要保留并重试。
    int result = avcodec_send_packet(context_.get(), packet);
    if (result == AVERROR(EAGAIN)) {
      Receive(false);
      result = avcodec_send_packet(context_.get(), packet);
    }
    CheckResult(result, "avcodec_send_packet");

    // receive 循环取得当前全部可用帧，不假设一个包刚好对应一帧。
    const int end = Receive(draining);
    if (draining && end != AVERROR_EOF) {
      throw std::runtime_error("decoder requested input after end of stream");
    }
  }

  void PrintSummary() const {
    std::cout << "decode_summary frames=" << frame_count_
              << " drained_frames=" << drained_frames_ << '\n';
  }

 private:
  int Receive(bool draining) {
    while (true) {
      const int result = avcodec_receive_frame(context_.get(), frame_.get());
      // EAGAIN 表示需要继续送包；EOF 表示结束信号后的结果已经取完。
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        return result;
      }
      CheckResult(result, "avcodec_receive_frame");

      const char* pixel_format =
          av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame_->format));
      std::cout << "frame index=" << frame_count_
                << " stream=" << stream_->index
                << " pts=" << Timestamp(frame_->pts)
                << " best_effort_timestamp="
                << Timestamp(frame_->best_effort_timestamp) << " time_s="
                << Seconds(frame_->best_effort_timestamp, stream_->time_base)
                << " width=" << frame_->width << " height=" << frame_->height
                << " pix_fmt=" << (pixel_format ? pixel_format : "unknown")
                << " linesize0=" << frame_->linesize[0]
                << " linesize1=" << frame_->linesize[1]
                << " linesize2=" << frame_->linesize[2]
                << " picture=" << av_get_picture_type_char(frame_->pict_type)
                << " phase=" << (draining ? "drain" : "packets") << '\n';
      if (frame_count_ == 0) {
        SaveFirstFrame(frame_.get(), image_path_);
      }
      ++frame_count_;
      if (draining) {
        ++drained_frames_;
      }
      // 图片转换和输出完成后释放像素引用，帧对象复用于下一次接收。
      av_frame_unref(frame_.get());
    }
  }

  const AVStream* stream_;
  std::string image_path_;
  std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> context_;
  FramePtr frame_;
  int64_t frame_count_ = 0;
  int64_t drained_frames_ = 0;
};

void Inspect(const std::string& filename, int packet_limit,
             const std::string& image_path = {}) {
  // 1. 打开容器。FFmpeg 创建 AVFormatContext，并把地址写入 raw_input。
  // 此时并没有把整个视频解码成图片。
  AVFormatContext* raw_input = nullptr;
  CheckResult(
      avformat_open_input(&raw_input, filename.c_str(), nullptr, nullptr),
      "avformat_open_input");

  // 关闭输入会一起释放流信息；异常路径也由 unique_ptr 保证调用。
  // FFmpeg 的释放接口接收二级指针，因此在删除器中传入局部指针的地址。
  const auto close_input = [](AVFormatContext* input) {
    avformat_close_input(&input);
  };
  std::unique_ptr<AVFormatContext, decltype(close_input)> input(raw_input,
                                                                close_input);

  // 2. 探测流。某些参数需要读取编码数据后才能确定，不能只依赖文件头。
  CheckResult(avformat_find_stream_info(input.get(), nullptr),
              "avformat_find_stream_info");

  // 容器总时长以 1/AV_TIME_BASE 秒为单位，不使用某条流的 time_base。
  std::cout << "format name=" << input->iformat->name
            << " streams=" << input->nb_streams << " duration_s="
            << Seconds(input->duration, AVRational{1, AV_TIME_BASE})
            << " bit_rate=" << input->bit_rate << '\n';

  // 3. 每个 AVStream 描述一条轨道；codecpar 描述其编码和音视频参数。
  // AVStream 和 codecpar 属于输入上下文，这里只借用，不单独释放。
  bool has_audio = false;
  const AVStream* video_stream = nullptr;
  for (unsigned int index = 0; index < input->nb_streams; ++index) {
    const AVStream* stream = input->streams[index];
    const AVCodecParameters* parameters = stream->codecpar;
    const char* type = av_get_media_type_string(parameters->codec_type);
    std::cout << "stream index=" << stream->index
              << " type=" << (type ? type : "unknown")
              << " codec=" << avcodec_get_name(parameters->codec_id)
              << " time_base=" << stream->time_base.num << '/'
              << stream->time_base.den;
    if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
      // 本示例选择遇到的第一条视频流，其索引不一定为 0。
      if (!video_stream) {
        video_stream = stream;
      }
      std::cout << " width=" << parameters->width
                << " height=" << parameters->height
                << " avg_frame_rate=" << stream->avg_frame_rate.num << '/'
                << stream->avg_frame_rate.den;
    } else if (parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
      has_audio = true;
      std::cout << " sample_rate=" << parameters->sample_rate
                << " channels=" << parameters->ch_layout.nb_channels;
    }
    std::cout << '\n';
  }
  // 无音轨是输入文件的一种正常情况。
  std::cout << "audio present=" << (has_audio ? "yes" : "no") << '\n';

  std::unique_ptr<VideoDecoder> decoder;
  if (!image_path.empty()) {
    if (!video_stream) {
      throw std::runtime_error("no video stream found");
    }
    decoder = std::make_unique<VideoDecoder>(video_stream, image_path);
  }

  // 4. AVPacket 是可复用的包对象。data 引用编码字节，pts/dts 等是元数据。
  const auto free_packet = [](AVPacket* packet) { av_packet_free(&packet); };
  std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(),
                                                          free_packet);
  if (!packet) {
    throw std::bad_alloc();
  }

  int64_t packet_count = 0;
  bool reached_eof = false;
  // 解码模式持续读取到 EOF；查看包的模式按指定数量停止。
  while (decoder || packet_count < packet_limit) {
    // av_read_frame 解封装出一个编码包，不会返回可显示的 AVFrame。
    // 音视频包可能交错返回；保持读取顺序，stream_index 标识包的归属。
    const int result = av_read_frame(input.get(), packet.get());
    if (result == AVERROR_EOF) {
      reached_eof = true;
      break;
    }
    CheckResult(result, "av_read_frame");

    const AVStream* stream = input->streams[packet->stream_index];
    const AVRational time_base = stream->time_base;
    std::cout << "packet index=" << packet_count
              << " stream=" << packet->stream_index << " size=" << packet->size
              << " pts=" << Timestamp(packet->pts)
              << " pts_s=" << Seconds(packet->pts, time_base)
              << " dts=" << Timestamp(packet->dts)
              << " dts_s=" << Seconds(packet->dts, time_base)
              << " duration=" << packet->duration
              << " duration_s=" << Seconds(packet->duration, time_base)
              << " key=" << ((packet->flags & AV_PKT_FLAG_KEY) ? "yes" : "no")
              << '\n';

    // 只有选中视频流的包进入这个解码实例；其他流的包在本阶段仅展示信息。
    if (decoder && packet->stream_index == video_stream->index) {
      decoder->Send(packet.get());
    }

    // unref 释放本次编码数据的引用，保留包对象供下一次读包使用。
    // key 只是包的关键帧标记，不能单靠它判断非关键包属于 P 帧还是 B 帧。
    av_packet_unref(packet.get());
    ++packet_count;
  }
  // eof=no 表示因数量上限停止读取，不代表文件一定还有数据。
  std::cout << "summary packets=" << packet_count
            << " eof=" << (reached_eof ? "yes" : "no") << '\n';
  if (decoder) {
    // 输入读完不等于解码器输出完：空指针是结束信号，不是一个媒体包。
    std::cout << "drain status=begin\n";
    decoder->Send(nullptr);
    std::cout << "drain status=end\n";
    decoder->PrintSummary();
  }
  // 5. 退出作用域依次释放包、解码实例、输入；异常返回也按此顺序释放。
}

#ifdef _WIN32
// Windows 命令行采用 UTF-16，FFmpeg 文件接口接收 UTF-8 路径。
std::string Argument(const wchar_t* value) {
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                                       nullptr, 0, nullptr, nullptr);
  if (size == 0) {
    throw std::runtime_error("invalid command-line text");
  }
  std::string result(size, '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                          result.data(), size, nullptr, nullptr) != size) {
    throw std::runtime_error("invalid command-line text");
  }
  result.pop_back();
  return result;
}
#else
std::string Argument(const char* value) { return value; }
#endif

int Usage() {
  std::cerr << "usage: media_info <file> [positive-packet-count]\n"
            << "       media_info <file> --decode <output.ppm>\n";
  return 2;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[])
#else
int main(int argc, char* argv[])
#endif
{
  try {
    if (argc == 4 && Argument(argv[2]) == "--decode") {
      const std::string image_path = Argument(argv[3]);
      if (image_path.empty()) {
        return Usage();
      }
      Inspect(Argument(argv[1]), 0, image_path);
      return 0;
    }
    if (argc < 2 || argc > 3) {
      return Usage();
    }
    int packet_limit = 8;
    if (argc == 3) {
      const std::string value = Argument(argv[2]);
      const auto result = std::from_chars(
          value.data(), value.data() + value.size(), packet_limit);
      if (result.ec != std::errc{} ||
          result.ptr != value.data() + value.size() || packet_limit <= 0) {
        return Usage();
      }
    }
    Inspect(Argument(argv[1]), packet_limit);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
