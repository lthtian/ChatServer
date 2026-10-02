#include "video_decoder.h"

#include <QFileInfo>
#include <QThread>
#include <algorithm>
#include <stdexcept>

namespace {
constexpr AVRational kMicroseconds{1, AV_TIME_BASE};

void Check(int result, const char* operation) {
  if (result >= 0) return;
  char text[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(result, text, sizeof(text));
  throw std::runtime_error(std::string(operation) + ": " + text);
}

int Interrupted(void*) {
  return QThread::currentThread()->isInterruptionRequested() ? 1 : 0;
}
}  // namespace

VideoDecoder::VideoDecoder()
    : input_(nullptr, [](AVFormatContext* p) { avformat_close_input(&p); }),
      context_(nullptr, [](AVCodecContext* p) { avcodec_free_context(&p); }),
      packet_(nullptr, [](AVPacket* p) { av_packet_free(&p); }),
      frame_(nullptr, [](AVFrame* p) { av_frame_free(&p); }),
      rgb_(nullptr, [](AVFrame* p) { av_frame_free(&p); }),
      scaler_(nullptr, [](SwsContext* p) { sws_free_context(&p); }) {}

void VideoDecoder::Open(const QString& filename, quint64 session) {
  session_ = session;
  context_.reset();
  input_.reset();
  stream_ = nullptr;
  draining_ = false;
  has_frame_ = false;
  next_pts_us_ = 0;
  try {
    if (!QFileInfo(filename).isFile()) {
      throw std::runtime_error("请选择存在的本地视频文件");
    }
    if (Interrupted(nullptr)) return;
    AVFormatContext* raw = avformat_alloc_context();
    if (!raw) throw std::bad_alloc();
    // 窗口关闭请求中断时，FFmpeg 的读文件操作也能检查该请求。
    raw->interrupt_callback = {Interrupted, nullptr};
    const int opened = avformat_open_input(&raw, filename.toUtf8().constData(),
                                           nullptr, nullptr);
    input_.reset(raw);
    Check(opened, "avformat_open_input");
    Check(avformat_find_stream_info(input_.get(), nullptr),
          "avformat_find_stream_info");
    for (unsigned int i = 0; i < input_->nb_streams; ++i) {
      if (input_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        stream_ = input_->streams[i];
        break;
      }
    }
    if (!stream_) throw std::runtime_error("文件中没有视频流");
    const AVCodec* codec = avcodec_find_decoder(stream_->codecpar->codec_id);
    if (!codec) throw std::runtime_error("找不到对应的视频解码器");
    context_.reset(avcodec_alloc_context3(codec));
    packet_.reset(av_packet_alloc());
    frame_.reset(av_frame_alloc());
    rgb_.reset(av_frame_alloc());
    scaler_.reset(sws_alloc_context());
    if (!context_ || !packet_ || !frame_ || !rgb_ || !scaler_) {
      throw std::bad_alloc();
    }
    Check(avcodec_parameters_to_context(context_.get(), stream_->codecpar),
          "avcodec_parameters_to_context");
    context_->pkt_timebase = stream_->time_base;
    context_->thread_count = 1;
    Check(avcodec_open2(context_.get(), codec, nullptr), "avcodec_open2");
    scaler_->flags = SWS_BILINEAR;
    scaler_->threads = 1;
    const AVRational rate = av_guess_frame_rate(
        input_.get(), input_->streams[stream_->index], nullptr);
    // 无帧时长时用帧率估计；帧率也未知才采用 25 fps 的明确兜底值。
    fallback_duration_us_ =
        rate.num > 0 && rate.den > 0
            ? std::max<int64_t>(1,
                                av_rescale_q(1, av_inv_q(rate), kMicroseconds))
            : 40000;
    qint64 duration = 0;
    if (stream_->duration != AV_NOPTS_VALUE && stream_->duration > 0) {
      duration =
          av_rescale_q(stream_->duration, stream_->time_base, kMicroseconds);
    }
    emit Opened(session_, duration);
  } catch (const std::exception& error) {
    emit Failed(session_, QString::fromUtf8(error.what()));
  }
}

void VideoDecoder::Read(quint64 session) {
  if (session != session_ || !context_ || Interrupted(nullptr)) return;
  try {
    while (!Interrupted(nullptr)) {
      // 每次请求只返回一帧；先取输出，需要更多输入时才继续读包。
      const int result = avcodec_receive_frame(context_.get(), frame_.get());
      if (result == 0) {
        qint64 duration = frame_->duration > 0
                              ? av_rescale_q(frame_->duration,
                                             stream_->time_base, kMicroseconds)
                              : fallback_duration_us_;
        duration = std::max<qint64>(1, duration);
        const qint64 pts =
            frame_->best_effort_timestamp == AV_NOPTS_VALUE
                ? next_pts_us_
                : av_rescale_q(frame_->best_effort_timestamp,
                               stream_->time_base, kMicroseconds);
        QImage image = ConvertFrame();
        next_pts_us_ = pts + duration;
        has_frame_ = true;
        av_frame_unref(frame_.get());
        emit FrameReady(session_, std::move(image), pts, duration);
        return;
      }
      if (result == AVERROR_EOF) {
        if (!has_frame_) throw std::runtime_error("视频没有可显示的帧");
        emit Ended(session_);
        return;
      }
      if (result != AVERROR(EAGAIN)) Check(result, "avcodec_receive_frame");
      if (draining_) throw std::runtime_error("解码器排空时仍请求输入");

      int read = 0;
      do {
        av_packet_unref(packet_.get());
        read = av_read_frame(input_.get(), packet_.get());
      } while (read >= 0 && packet_->stream_index != stream_->index &&
               !Interrupted(nullptr));
      if (Interrupted(nullptr)) return;
      if (read == AVERROR_EOF) {
        Check(avcodec_send_packet(context_.get(), nullptr),
              "avcodec_send_packet");
        draining_ = true;
      } else {
        Check(read, "av_read_frame");
        // receive 已返回 EAGAIN，按 API 契约此时可以继续提交输入。
        Check(avcodec_send_packet(context_.get(), packet_.get()),
              "avcodec_send_packet");
        av_packet_unref(packet_.get());
      }
    }
  } catch (const std::exception& error) {
    emit Failed(session_, QString::fromUtf8(error.what()));
  }
}

QImage VideoDecoder::ConvertFrame() {
  av_frame_unref(rgb_.get());
  rgb_->format = AV_PIX_FMT_RGB24;
  rgb_->width = frame_->width;
  rgb_->height = frame_->height;
  rgb_->color_range = AVCOL_RANGE_JPEG;
  rgb_->colorspace = AVCOL_SPC_RGB;
  rgb_->color_primaries = frame_->color_primaries;
  rgb_->color_trc = frame_->color_trc;
  Check(sws_scale_frame(scaler_.get(), rgb_.get(), frame_.get()),
        "sws_scale_frame");
  // 此构造函数只借用 FFmpeg 像素；copy 使跨线程传递的图像拥有独立内存。
  QImage image(rgb_->data[0], rgb_->width, rgb_->height, rgb_->linesize[0],
               QImage::Format_RGB888);
  QImage owned = image.copy();
  if (owned.isNull()) throw std::bad_alloc();
  return owned;
}
