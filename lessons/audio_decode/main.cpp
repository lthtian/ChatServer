#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace {
constexpr int kOutputRate = 48000;
constexpr int kOutputChannels = 2;
constexpr AVSampleFormat kOutputFormat = AV_SAMPLE_FMT_S16;
static_assert(Q_BYTE_ORDER == Q_LITTLE_ENDIAN,
              "PCM output requires little endian");

void Check(int result, const char* operation) {
  if (result >= 0) return;
  char message[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(result, message, sizeof(message));
  throw std::runtime_error(std::string(operation) + ": " + message);
}

std::string Seconds(int64_t timestamp, AVRational time_base) {
  if (timestamp == AV_NOPTS_VALUE) return "unknown";
  std::ostringstream text;
  text << std::fixed << std::setprecision(6) << timestamp * av_q2d(time_base);
  return text.str();
}

// 持续解码一条音轨；转换器保留跨音频帧的滤波历史和暂未输出的采样。
class AudioDecoder {
 public:
  AudioDecoder(const AVStream* stream, const QString& output_path)
      : stream_(stream),
        output_(output_path),
        context_(nullptr, [](AVCodecContext* p) { avcodec_free_context(&p); }),
        frame_(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); }),
        resampler_(nullptr, [](SwrContext* p) { swr_free(&p); }) {
    const AVCodec* codec = avcodec_find_decoder(stream_->codecpar->codec_id);
    if (!codec) throw std::runtime_error("audio decoder not found");
    context_.reset(avcodec_alloc_context3(codec));
    if (!context_ || !frame_) throw std::bad_alloc();
    Check(avcodec_parameters_to_context(context_.get(), stream_->codecpar),
          "avcodec_parameters_to_context");
    context_->pkt_timebase = stream_->time_base;
    context_->thread_count = 1;
    Check(avcodec_open2(context_.get(), codec, nullptr), "avcodec_open2");
    // 排他创建输出；输入文件与任何已有文件均不能被覆盖。
    if (QFileInfo::exists(output_path)) {
      throw std::runtime_error("output file already exists");
    }
    if (!output_.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
      throw std::runtime_error("cannot create output file");
    }
    std::cout << "audio stream=" << stream_->index << " codec=" << codec->name
              << " time_base=" << stream_->time_base.num << '/'
              << stream_->time_base.den << '\n';
    std::cout << "target sample_rate=" << kOutputRate
              << " channels=" << kOutputChannels
              << " sample_fmt=s16 layout=stereo byte_order=little\n";
  }

  ~AudioDecoder() { av_channel_layout_uninit(&source_layout_); }

  void Send(const AVPacket* packet) {
    int result = avcodec_send_packet(context_.get(), packet);
    if (result == AVERROR(EAGAIN)) {
      // 此包尚未接受：先取出已有结果，再提交同一个包。
      Receive(false);
      result = avcodec_send_packet(context_.get(), packet);
    }
    Check(result, "avcodec_send_packet");
    const int received = Receive(packet == nullptr);
    if (!packet) {
      if (received != AVERROR_EOF) {
        throw std::runtime_error("decoder requested input while draining");
      }
      decoder_eof_ = true;
    }
  }

  void Finish() {
    if (!decoder_eof_) throw std::runtime_error("decoder has not reached EOF");
    if (frame_count_ == 0)
      throw std::runtime_error("audio stream contains no samples");
    // 解码器排空之后，重采样器仍可能保留尾部采样；空输入一直取到返回 0。
    int produced = 0;
    while ((produced = Convert(nullptr)) > 0) tail_samples_ += produced;
    if (!output_.flush()) throw std::runtime_error("cannot write output file");
    output_.close();
    std::cout << "summary frames=" << frame_count_
              << " input_samples=" << input_samples_
              << " output_samples=" << output_samples_ << " output_bytes="
              << output_samples_ * kOutputChannels * sizeof(int16_t)
              << " resampler_tail_samples=" << tail_samples_
              << " decoder_eof=yes duration_s=" << std::fixed
              << std::setprecision(6)
              << static_cast<double>(output_samples_) / kOutputRate << '\n';
  }

 private:
  int Receive(bool draining) {
    while (true) {
      const int result = avcodec_receive_frame(context_.get(), frame_.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return result;
      Check(result, "avcodec_receive_frame");
      PrepareResampler();
      const auto format = static_cast<AVSampleFormat>(frame_->format);
      const int64_t pts = frame_->best_effort_timestamp;
      char layout[128] = {};
      Check(av_channel_layout_describe(&frame_->ch_layout, layout,
                                       sizeof(layout)),
            "av_channel_layout_describe");
      std::cout << "frame index=" << frame_count_ << " pts="
                << (pts == AV_NOPTS_VALUE ? "unknown" : std::to_string(pts))
                << " time_s=" << Seconds(pts, stream_->time_base)
                << " nb_samples=" << frame_->nb_samples
                << " sample_rate=" << frame_->sample_rate
                << " channels=" << frame_->ch_layout.nb_channels
                << " layout=" << layout
                << " sample_fmt=" << av_get_sample_fmt_name(format)
                << " planar="
                << (av_sample_fmt_is_planar(format) ? "yes" : "no")
                << " bytes_per_sample=" << av_get_bytes_per_sample(format)
                << " linesize0=" << frame_->linesize[0]
                << " phase=" << (draining ? "drain" : "packets") << '\n';
      Convert(frame_.get());
      input_samples_ += frame_->nb_samples;
      ++frame_count_;
      // 释放本帧引用，保留 AVFrame 对象供下一次 receive 使用。
      av_frame_unref(frame_.get());
    }
  }

  void PrepareResampler() {
    const auto format = static_cast<AVSampleFormat>(frame_->format);
    if (frame_->sample_rate <= 0 || frame_->nb_samples <= 0 ||
        !av_channel_layout_check(&frame_->ch_layout) ||
        av_get_bytes_per_sample(format) <= 0) {
      throw std::runtime_error("invalid audio frame parameters");
    }
    if (resampler_) {
      if (source_rate_ != frame_->sample_rate || source_format_ != format ||
          av_channel_layout_compare(&source_layout_, &frame_->ch_layout) != 0) {
        throw std::runtime_error("audio format changed within the stream");
      }
      return;
    }
    // 用第一帧的实际参数配置转换器；codec_id 不能决定解码后的 PCM 布局。
    source_rate_ = frame_->sample_rate;
    source_format_ = format;
    Check(av_channel_layout_copy(&source_layout_, &frame_->ch_layout),
          "av_channel_layout_copy");
    const AVChannelLayout target_layout = AV_CHANNEL_LAYOUT_STEREO;
    SwrContext* raw = nullptr;
    const int result = swr_alloc_set_opts2(
        &raw, &target_layout, kOutputFormat, kOutputRate, &source_layout_,
        source_format_, source_rate_, 0, nullptr);
    resampler_.reset(raw);
    Check(result, "swr_alloc_set_opts2");
    Check(swr_init(resampler_.get()), "swr_init");
  }

  int Convert(const AVFrame* source) {
    const int input_count = source ? source->nb_samples : 0;
    // 上界计入转换器当前的缓存；容量、输入量和返回值均按每个声道计数。
    const int bound = swr_get_out_samples(resampler_.get(), input_count);
    Check(bound, "swr_get_out_samples");
    const int capacity = std::max(1, bound);
    pcm_.resize(static_cast<size_t>(capacity) * kOutputChannels);
    uint8_t* output_data = reinterpret_cast<uint8_t*>(pcm_.data());
    // S16 是交错格式，只有一个输出平面；extended_data 支持平面或交错输入。
    const int produced =
        swr_convert(resampler_.get(), &output_data, capacity,
                    source ? source->extended_data : nullptr, input_count);
    Check(produced, "swr_convert");
    const qint64 bytes =
        static_cast<qint64>(produced) * kOutputChannels * sizeof(int16_t);
    // 只写实际产生的有效字节，不能把整个预分配容量写进 PCM 文件。
    if (output_.write(reinterpret_cast<const char*>(pcm_.data()), bytes) !=
        bytes) {
      throw std::runtime_error("cannot write output file");
    }
    output_samples_ += produced;
    std::cout << "convert in_samples=" << input_count
              << " out_samples=" << produced << " delay_samples="
              << swr_get_delay(resampler_.get(), kOutputRate)
              << " phase=" << (source ? "frames" : "drain") << '\n';
    return produced;
  }

  const AVStream* stream_;
  QFile output_;
  std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> context_;
  std::unique_ptr<AVFrame, void (*)(AVFrame*)> frame_;
  std::unique_ptr<SwrContext, void (*)(SwrContext*)> resampler_;
  AVChannelLayout source_layout_{};
  int source_rate_ = 0;
  AVSampleFormat source_format_ = AV_SAMPLE_FMT_NONE;
  std::vector<int16_t> pcm_;
  int64_t frame_count_ = 0;
  int64_t input_samples_ = 0;
  int64_t output_samples_ = 0;
  int64_t tail_samples_ = 0;
  bool decoder_eof_ = false;
};

void Decode(const QString& input_path, const QString& output_path) {
  if (!QFileInfo(input_path).isFile()) {
    throw std::runtime_error("input file does not exist");
  }
  AVFormatContext* raw = nullptr;
  Check(avformat_open_input(&raw, input_path.toUtf8().constData(), nullptr,
                            nullptr),
        "avformat_open_input");
  const auto close_input = [](AVFormatContext* p) { avformat_close_input(&p); };
  std::unique_ptr<AVFormatContext, decltype(close_input)> input(raw,
                                                                close_input);
  Check(avformat_find_stream_info(input.get(), nullptr),
        "avformat_find_stream_info");
  const AVStream* audio = nullptr;
  for (unsigned int i = 0; i < input->nb_streams; ++i) {
    if (input->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      audio = input->streams[i];
      break;
    }
  }
  if (!audio) throw std::runtime_error("no audio stream");
  AudioDecoder decoder(audio, output_path);
  const auto free_packet = [](AVPacket* p) { av_packet_free(&p); };
  std::unique_ptr<AVPacket, decltype(free_packet)> packet(av_packet_alloc(),
                                                          free_packet);
  if (!packet) throw std::bad_alloc();
  while (true) {
    const int result = av_read_frame(input.get(), packet.get());
    if (result == AVERROR_EOF) break;
    Check(result, "av_read_frame");
    // 一个文件可有多条轨道；本例只解码第一条音轨，视频包直接释放。
    if (packet->stream_index == audio->index) decoder.Send(packet.get());
    av_packet_unref(packet.get());
  }
  decoder.Send(nullptr);
  decoder.Finish();
}
}  // namespace

int main(int argc, char* argv[]) {
  // Qt Core 只处理命令行与文件路径；这个控制台示例不启动 GUI 或播放事件循环。
  QCoreApplication app(argc, argv);
  const QStringList arguments = app.arguments();
  if (arguments.size() != 3) {
    std::cerr << "usage: audio_decode <input> <output.pcm>\n";
    return 2;
  }
  try {
    Decode(arguments[1], arguments[2]);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}
