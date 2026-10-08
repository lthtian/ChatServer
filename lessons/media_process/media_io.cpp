#include "media_io.h"

#include <QFile>
#include <QFileInfo>
#include <stdexcept>

namespace media {

void Check(int result, const char* operation) {
  if (result >= 0) return;
  char text[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(result, text, sizeof(text));
  throw std::runtime_error(std::string(operation) + ": " + text);
}

InputPtr OpenInput(const std::string& path, const char* format) {
  if (!QFileInfo(QString::fromStdString(path)).isFile())
    throw std::runtime_error("input file does not exist");
  AVFormatContext* raw = nullptr;
  AVDictionary* options = nullptr;
  av_dict_set(&options, "protocol_whitelist", "file", 0);
  // 服务端 HLS 入口明确使用 mov 解封装器，上传的文本不能被当作远程播放列表执行。
  const int opened = avformat_open_input(&raw, path.c_str(), format ? av_find_input_format(format) : nullptr, &options);
  av_dict_free(&options);
  Check(opened, "avformat_open_input");
  InputPtr input(raw, [](AVFormatContext* p) { avformat_close_input(&p); });
  Check(avformat_find_stream_info(input.get(), nullptr), "avformat_find_stream_info");
  return input;
}

CodecPtr CreateCodec(const AVCodec* codec) {
  if (!codec) throw std::runtime_error("required codec is unavailable");
  CodecPtr value(avcodec_alloc_context3(codec),
                 [](AVCodecContext* p) { avcodec_free_context(&p); });
  if (!value) throw std::bad_alloc();
  return value;
}

FramePtr CreateFrame() {
  FramePtr value(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); });
  if (!value) throw std::bad_alloc();
  return value;
}

PacketPtr CreatePacket() {
  PacketPtr value(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
  if (!value) throw std::bad_alloc();
  return value;
}

Output::Output(const std::string& path)
    : path_(path), temporary_path_(path + ".part"),
      context_(nullptr, [](AVFormatContext* p) {
        if (p) {
          if (p->pb) avio_closep(&p->pb);
          avformat_free_context(p);
        }
      }) {
  if (QFileInfo::exists(QString::fromStdString(path_)) ||
      QFileInfo::exists(QString::fromStdString(temporary_path_)))
    throw std::runtime_error("output or .part file already exists: " + path_);
  const auto extension = QFileInfo(QString::fromStdString(path_)).suffix().toLower();
  const char* format = extension == "mp4" ? "mp4" : extension == "mkv" ? "matroska" : nullptr;
  if (!format) throw std::runtime_error("output extension must be .mp4 or .mkv");
  AVFormatContext* raw = nullptr;
  const int result = avformat_alloc_output_context2(&raw, nullptr, format,
                                                  temporary_path_.c_str());
  context_.reset(raw);
  Check(result, "avformat_alloc_output_context2");
  if (!context_) throw std::bad_alloc();
}

Output::~Output() {
  context_.reset();  // 先关闭磁盘句柄，Windows 下才能删除半成品。
  if (opened_) {
    QFile::remove(QString::fromStdString(temporary_path_));
  }
}

AVStream* Output::AddStream() {
  AVStream* stream = avformat_new_stream(context_.get(), nullptr);
  if (!stream) throw std::bad_alloc();
  return stream;
}

void Output::WriteHeader() {
  QFile reservation(QString::fromStdString(temporary_path_));
  if (!reservation.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    throw std::runtime_error("cannot create output .part file");
  opened_ = true;
  reservation.close();
  Check(avio_open(&context_->pb, temporary_path_.c_str(), AVIO_FLAG_WRITE), "avio_open");
  AVDictionary* options = nullptr;
  if (std::string(context_->oformat->name) == "mp4")
    Check(av_dict_set(&options, "movflags", "+faststart", 0), "av_dict_set");
  // faststart 在写尾时把 moov 索引移到文件头，便于 HTTP 点播尽早取得索引。
  const int result = avformat_write_header(context_.get(), &options);
  av_dict_free(&options);
  Check(result, "avformat_write_header");
}

void Output::WritePacket(AVPacket* packet, AVRational source_time_base, AVStream* stream) {
  // 必须使用写头之后的输出 time_base；不要沿用创建轨道时的猜测值。
  av_packet_rescale_ts(packet, source_time_base, stream->time_base);
  packet->stream_index = stream->index;
  packet->pos = -1;
  Check(av_interleaved_write_frame(context_.get(), packet), "av_interleaved_write_frame");
}

void Output::Finish() {
  Check(av_write_trailer(context_.get()), "av_write_trailer");
  Check(avio_closep(&context_->pb), "avio_closep");
  if (!QFile::rename(QString::fromStdString(temporary_path_), QString::fromStdString(path_)))
    throw std::runtime_error("cannot publish output file (destination may already exist)");
  opened_ = false;
}

}  // namespace media
