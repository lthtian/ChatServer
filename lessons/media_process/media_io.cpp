#include "media_io.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <chrono>
#include <cstring>
#include <stdexcept>

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace media {

namespace {
QJsonValue Ratio(AVRational ratio) {
  return ratio.num > 0 && ratio.den > 0 ? QJsonValue(QJsonArray{ratio.num, ratio.den}) : QJsonValue();
}
QJsonValue Name(const char* value) { return value ? QJsonValue(QString::fromUtf8(value)) : QJsonValue(); }
QJsonValue Positive(int value) { return value > 0 ? QJsonValue(value) : QJsonValue(); }
}

void Probe(const std::string& input_path, const std::string& output_path) {
  QJsonObject result{{"schema", 1}, {"state", "unrecognized"}, {"failure", QJsonValue()},
      {"demuxer", QJsonValue()}, {"container", QJsonValue()}, {"duration_us", QJsonValue()},
      {"video_stream", QJsonValue()}, {"audio_stream", QJsonValue()}, {"tracks", QJsonArray()}};
  try {
    const QFileInfo file(QString::fromStdString(input_path));
    if (!file.isFile() || file.size() <= 0 || file.size() > 100 * 1024 * 1024)
      throw std::runtime_error("file_size_or_type");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    AVIOInterruptCB interrupt{[](void* opaque) {
      return int(std::chrono::steady_clock::now() >=
                 *static_cast<const std::chrono::steady_clock::time_point*>(opaque));
    }, &deadline};
    AVIOContext* io = nullptr;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    const int opened = avio_open2(&io, input_path.c_str(), AVIO_FLAG_READ, &interrupt, &options);
    av_dict_free(&options);
    std::unique_ptr<AVIOContext, void (*)(AVIOContext*)> bytes(io, [](AVIOContext* p) { if (p) avio_closep(&p); });
    Check(opened, "open_file");
    AVFormatContext* raw = avformat_alloc_context();
    if (!raw) throw std::bad_alloc();
    raw->pb = io;
    raw->flags |= AVFMT_FLAG_CUSTOM_IO;
    raw->interrupt_callback = interrupt;
    // 已经打开唯一原件；解封装器不能再打开文件、网络或播放列表引用。
    raw->io_open = [](AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) {
      return AVERROR(EACCES);
    };
    av_dict_set(&options, "format_whitelist", "mov,matroska,webm,avi,mpegts,flv,ogg", 0);
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "enable_drefs", "0", 0);
    av_dict_set(&options, "probesize", "1048576", 0);
    av_dict_set(&options, "analyzeduration", "2000000", 0);
    av_dict_set(&options, "max_streams", "64", 0);
    const int format = avformat_open_input(&raw, nullptr, nullptr, &options);
    av_dict_free(&options);
    InputPtr input(raw, [](AVFormatContext* p) { avformat_close_input(&p); });
    Check(format, "probe_container");
    Check(avformat_find_stream_info(input.get(), nullptr), "probe_tracks");
    result["container"] = Name(input->iformat->name);
    result["demuxer"] = QString::fromUtf8(input->iformat->name).section(',', 0, 0);
    if (input->duration != AV_NOPTS_VALUE && input->duration >= 0)
      result["duration_us"] = double(input->duration);
    QJsonArray tracks;
    for (unsigned i = 0; i < input->nb_streams; ++i) {
      AVStream* stream = input->streams[i];
      const auto* p = stream->codecpar;
      QJsonObject track{{"index", stream->index}, {"type", Name(av_get_media_type_string(p->codec_type))},
          {"codec", Name(avcodec_get_name(p->codec_id))}, {"profile", Name(avcodec_profile_name(p->codec_id, p->profile))},
          {"time_base", Ratio(stream->time_base)}, {"decoder_available", avcodec_find_decoder(p->codec_id) != nullptr},
          {"attached_picture", bool(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)}};
      track["start_us"] = stream->start_time == AV_NOPTS_VALUE ? QJsonValue() :
          QJsonValue(double(av_rescale_q(stream->start_time, stream->time_base, AVRational{1, AV_TIME_BASE})));
      track["duration_us"] = stream->duration == AV_NOPTS_VALUE ? QJsonValue() :
          QJsonValue(double(av_rescale_q(stream->duration, stream->time_base, AVRational{1, AV_TIME_BASE})));
      if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
        track["width"] = Positive(p->width); track["height"] = Positive(p->height);
        track["pixel_format"] = Name(av_get_pix_fmt_name(static_cast<AVPixelFormat>(p->format)));
        const auto* pixels = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(p->format));
        track["bit_depth"] = pixels && pixels->nb_components > 0 ? Positive(pixels->comp[0].depth) : QJsonValue();
        track["average_frame_rate"] = Ratio(stream->avg_frame_rate);
        track["nominal_frame_rate"] = Ratio(stream->r_frame_rate);
        track["sar"] = Ratio(av_guess_sample_aspect_ratio(input.get(), stream, nullptr));
        track["display_matrix"] = QJsonValue();
        const auto* matrix = av_packet_side_data_get(p->coded_side_data, p->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
        if (matrix) {
          if (matrix->size < 9 * sizeof(int32_t)) throw std::runtime_error("invalid_display_matrix");
          int32_t values[9]; std::memcpy(values, matrix->data, sizeof(values));
          QJsonArray items; for (int value : values) items.append(value);
          track["display_matrix"] = items;
        }
        if (result["video_stream"].isNull() && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
          result["video_stream"] = stream->index;
      } else if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
        track["sample_rate"] = Positive(p->sample_rate);
        track["channels"] = Positive(p->ch_layout.nb_channels);
        char layout[128]{};
        track["channel_layout"] = av_channel_layout_describe(&p->ch_layout, layout, sizeof(layout)) >= 0 ? Name(layout) : QJsonValue();
        if (result["audio_stream"].isNull()) result["audio_stream"] = stream->index;
      }
      tracks.append(track);
    }
    result["tracks"] = tracks;
    result["state"] = result["video_stream"].isNull() ? "not_video" : "recognized";
    if (result["video_stream"].isNull()) result["failure"] = "no_video_track";
  } catch (const std::exception& error) {
    result["state"] = "unrecognized";
    result["video_stream"] = result["audio_stream"] = QJsonValue();
    result["failure"] = QString::fromUtf8(error.what());
  }
  QSaveFile output(QString::fromStdString(output_path));
  const QByteArray content = QJsonDocument(result).toJson(QJsonDocument::Compact);
  if (!output.open(QIODevice::WriteOnly) || output.write(content) != content.size() || !output.commit())
    throw std::runtime_error("cannot save media description");
}

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
  const char* format = extension == "mp4" ? "mp4" : extension == "mkv" ? "matroska"
      : extension == "ts" ? "mpegts" : nullptr;
  if (!format) throw std::runtime_error("output extension must be .mp4, .mkv or .ts");
  AVFormatContext* raw = nullptr;
  const int result = avformat_alloc_output_context2(&raw, nullptr, format,
                                                  temporary_path_.c_str());
  context_.reset(raw);
  Check(result, "avformat_alloc_output_context2");
  if (!context_) throw std::bad_alloc();
  // MPEG-TS 封装器按实际包格式自动接入 h264_mp4toannexb；调用方不再叠加同一 BSF。
  context_->flags |= AVFMT_FLAG_AUTO_BSF;
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
