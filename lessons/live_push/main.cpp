#include <algorithm>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>
}

namespace {
constexpr AVRational kMicroseconds{1, 1000000};
volatile std::sig_atomic_t stopping = 0;  // Ctrl+C 只设置标记，在正常控制流中退出。

// 信号处理函数不执行文件、网络或日志操作。
void Stop(int) { stopping = 1; }

// FFmpeg 负数返回值转成可读错误；正常 EOF 由读包循环单独处理。
void Check(int result, const char* operation) {
  if (result >= 0) return;
  char message[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(result, message, sizeof(message));
  throw std::runtime_error(std::string(operation) + ": " + message);
}

struct IoLimit {
  int64_t deadline_us = 0;  // 本次 I/O 的单调时钟截止时间；0 表示未设置。
  // 每次阻塞 I/O 最多等待十秒；这是操作超时，不是直播总时长。
  void Begin() { deadline_us = av_gettime_relative() + 10 * AV_TIME_BASE; }
};

// 由 FFmpeg 在可中断的 I/O 等待中调用，支持退出和网络超时。
int Interrupted(void* opaque) {
  const auto* limit = static_cast<const IoLimit*>(opaque);
  return stopping || (limit->deadline_us > 0 && av_gettime_relative() >= limit->deadline_us);
}

// 数据读取可能很快，发送必须等待媒体时刻；分段睡眠使 Ctrl+C 能及时生效。
void WaitUntil(int64_t due_us) {
  while (!stopping) {
    const int64_t remaining = due_us - av_gettime_relative();
    if (remaining <= 0) return;
    av_usleep(static_cast<unsigned int>(std::min<int64_t>(remaining, 20000)));
  }
}

// 单次推流：只复制一条 H.264 视频和可选的一条 AAC 音频，不解码、不转码、不循环。
void Push(const std::string& path, const std::string& url) {
  if (url.rfind("rtmp://", 0) != 0)
    throw std::runtime_error("output must be rtmp://host[:port]/app/stream");

  // 输入限制为本地 MP4；协议白名单防止把路径误当成网络输入。
  AVFormatContext* raw_input = nullptr;
  AVDictionary* options = nullptr;
  av_dict_set(&options, "protocol_whitelist", "file", 0);
  const int opened = avformat_open_input(&raw_input, path.c_str(), av_find_input_format("mov"), &options);
  av_dict_free(&options);
  std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> input(
      raw_input, [](AVFormatContext* p) { avformat_close_input(&p); });
  Check(opened, "open MP4");
  Check(avformat_find_stream_info(input.get(), nullptr), "find streams");
  const int video = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  Check(video, "find video");
  const int audio = av_find_best_stream(input.get(), AVMEDIA_TYPE_AUDIO, -1, video, nullptr, 0);
  if (audio < 0 && audio != AVERROR_STREAM_NOT_FOUND) Check(audio, "find audio");
  if (input->streams[video]->codecpar->codec_id != AV_CODEC_ID_H264 ||
      (audio >= 0 && input->streams[audio]->codecpar->codec_id != AV_CODEC_ID_AAC))
    throw std::runtime_error("sample requires H.264 video and optional AAC audio; transcode separately");

  // FLV 决定如何封装，rtmp:// 决定如何传输；RTMP 消息/chunk 由 FFmpeg 协议模块处理。
  IoLimit limit;
  AVFormatContext* raw_output = nullptr;
  Check(avformat_alloc_output_context2(&raw_output, nullptr, "flv", url.c_str()), "allocate FLV");
  std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> output(
      raw_output, [](AVFormatContext* p) {
        if (!p) return;
        if (p->pb) avio_closep(&p->pb);
        avformat_free_context(p);
      });
  if (!output) throw std::bad_alloc();
  output->interrupt_callback = {Interrupted, &limit};
  output->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;
  std::vector<int> mapping(input->nb_streams, -1);
  for (unsigned int i = 0; i < input->nb_streams; ++i) {
    if (static_cast<int>(i) != video && static_cast<int>(i) != audio) continue;
    const auto* source = input->streams[i];
    auto* target = avformat_new_stream(output.get(), nullptr);
    if (!target) throw std::bad_alloc();
    Check(avcodec_parameters_copy(target->codecpar, source->codecpar), "copy codec parameters");
    target->codecpar->codec_tag = 0;
    target->time_base = source->time_base;
    mapping[i] = target->index;
    std::cout << "stream=" << i << " type=" << (static_cast<int>(i) == video ? "video/h264" : "audio/aac")
              << " input_time_base=" << source->time_base.num << '/' << source->time_base.den << '\n';
  }

  limit.Begin();
  av_dict_set(&options, "rw_timeout", "10000000", 0);
  const int connected = avio_open2(&output->pb, url.c_str(), AVIO_FLAG_WRITE,
                                 &output->interrupt_callback, &options);
  av_dict_free(&options);
  Check(connected, "connect RTMP");
  // 直播输出不可回头改写时长和大小；不在结束时尝试回写这两项元数据。
  av_dict_set(&options, "flvflags", "no_duration_filesize", 0);
  limit.Begin();
  const int header = avformat_write_header(output.get(), &options);
  av_dict_free(&options);
  Check(header, "write FLV header");

  std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet(
      av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
  if (!packet) throw std::bad_alloc();
  std::vector<int64_t> last_dts(input->nb_streams, AV_NOPTS_VALUE);
  int64_t first_dts_us = AV_NOPTS_VALUE;  // 两轨共用发送时钟起点，不分别归零音视频。
  int64_t wall_start_us = 0;             // 第一包开始发送时的单调时钟。
  int64_t next_log_us = 0;               // 每秒打印一次进度，避免逐包日志影响观察。
  int64_t packets = 0, payload_bytes = 0;  // 编码包计数和净载荷字节数，不含网络协议开销。
  std::cout << "connected; publishing once (Ctrl+C to stop)" << std::endl;
  while (!stopping) {
    const int read = av_read_frame(input.get(), packet.get());
    if (read == AVERROR_EOF) break;
    Check(read, "read packet");
    const int index = packet->stream_index;
    if (mapping[index] < 0) { av_packet_unref(packet.get()); continue; }
    if (packet->dts == AV_NOPTS_VALUE || packet->pts == AV_NOPTS_VALUE)
      throw std::runtime_error("sample requires packet DTS and PTS");
    if (last_dts[index] != AV_NOPTS_VALUE && packet->dts < last_dts[index])
      throw std::runtime_error("input DTS moved backwards within a stream");
    last_dts[index] = packet->dts;
    const auto time_base = input->streams[index]->time_base;
    const int64_t dts_us = av_rescale_q(packet->dts, time_base, kMicroseconds);
    if (first_dts_us == AV_NOPTS_VALUE) {
      first_dts_us = dts_us;
      wall_start_us = av_gettime_relative();
    }
    // 使用 DTS 控制发送节奏；保留包内 PTS-DTS，供接收端按正确顺序解码和显示。
    const int64_t media_us = dts_us - first_dts_us;
    WaitUntil(wall_start_us + media_us);
    if (stopping) break;
    const int size = packet->size;
    packet->stream_index = mapping[index];
    av_packet_rescale_ts(packet.get(), time_base, output->streams[packet->stream_index]->time_base);
    packet->pos = -1;
    limit.Begin();
    Check(av_interleaved_write_frame(output.get(), packet.get()), "send media packet");
    // write_frame 已转移包引用；unref 保持每轮结束为空包。
    av_packet_unref(packet.get());
    ++packets;
    payload_bytes += size;
    if (media_us >= next_log_us) {
      std::cout << "media_s=" << media_us / 1000000.0
                << " elapsed_s=" << (av_gettime_relative() - wall_start_us) / 1000000.0
                << " packets=" << packets << " payload_bytes=" << payload_bytes << std::endl;
      next_log_us = media_us + AV_TIME_BASE;
    }
  }
  if (!stopping) {
    if (packets == 0) throw std::runtime_error("input contains no media packets");
    limit.Begin();
    Check(av_write_trailer(output.get()), "finish FLV");
  }
  std::cout << (stopping ? "stopped" : "completed") << " packets=" << packets
            << " payload_bytes=" << payload_bytes << std::endl;
}

#ifdef _WIN32
// Windows 使用宽字符命令行，转换成 FFmpeg 接收的 UTF-8 路径。
std::string Argument(const wchar_t* value) {
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 0) throw std::runtime_error("invalid command-line text");
  std::string result(size, '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, result.data(), size, nullptr, nullptr))
    throw std::runtime_error("cannot convert command-line text");
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
  if (argc != 3) {
    std::cerr << "usage: live_push input.mp4 rtmp://host[:port]/app/stream\n";
    return 2;
  }
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  av_log_set_level(AV_LOG_WARNING);
  int result = 0;
  try {
    Check(avformat_network_init(), "initialize network");
    Push(Argument(argv[1]), Argument(argv[2]));
  } catch (const std::exception& error) {
    std::cerr << (stopping ? "stopped: " : "error: ") << error.what() << '\n';
    result = stopping ? 0 : 1;
  }
  avformat_network_deinit();
  return result;
}
