#include "video_decoder.h"

#include <cstring>
#include "rtp.h"

namespace lesson {
VideoDecoder::VideoDecoder() {
  const AVCodec* implementation = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!implementation) throw std::runtime_error("H.264 decoder unavailable");
  codec_.reset(avcodec_alloc_context3(implementation));
  if (!codec_ || !packet_ || !frame_) throw std::bad_alloc();
  codec_->pkt_timebase = AVRational{1, kClockRate};
  codec_->thread_count = 1;
  CheckAv(avcodec_open2(codec_.get(), implementation, nullptr), "avcodec_open2");
}

VideoDecoder::~VideoDecoder() { sws_freeContext(scaler_); }

void VideoDecoder::Reset() { avcodec_flush_buffers(codec_.get()); }

void VideoDecoder::Decode(const QByteArray& bytes, qint64 timestamp, const Picture& picture) {
  av_packet_unref(packet_.get());
  // av_new_packet 还分配解码器要求的尾部 padding，不能直接借用任意 QByteArray 尾部。
  CheckAv(av_new_packet(packet_.get(), bytes.size()), "av_new_packet");
  std::memcpy(packet_->data, bytes.constData(), std::size_t(bytes.size()));
  packet_->pts = timestamp;
  packet_->dts = AV_NOPTS_VALUE;  // RTP 头没有单独的 DTS 字段。
  int result = avcodec_send_packet(codec_.get(), packet_.get());
  if (result == AVERROR(EAGAIN)) {
    Receive(picture);
    result = avcodec_send_packet(codec_.get(), packet_.get());
  }
  CheckAv(result, "avcodec_send_packet");
  av_packet_unref(packet_.get());
  Receive(picture);
}

void VideoDecoder::Receive(const Picture& picture) {
  while (true) {
    const int result = avcodec_receive_frame(codec_.get(), frame_.get());
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return;
    CheckAv(result, "avcodec_receive_frame");
    if (frame_->width <= 0 || frame_->height <= 0 ||
        qint64(frame_->width) * frame_->height > 3840 * 2160)
      throw std::runtime_error("decoded picture exceeds the lesson size limit");
    if (frame_->best_effort_timestamp == AV_NOPTS_VALUE)
      throw std::runtime_error("decoded picture has no timestamp");
    scaler_ = sws_getCachedContext(scaler_, frame_->width, frame_->height,
        AVPixelFormat(frame_->format), frame_->width, frame_->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!scaler_) throw std::runtime_error("sws_getCachedContext failed");
    // 像素格式与颜色矩阵是两回事：按帧元数据选择 YUV→RGB 系数及输入范围。
    int matrix = SWS_CS_DEFAULT;
    if (frame_->colorspace == AVCOL_SPC_BT709) matrix = SWS_CS_ITU709;
    if (frame_->colorspace == AVCOL_SPC_BT2020_NCL) matrix = SWS_CS_BT2020;
    const int* coefficients = sws_getCoefficients(matrix);
    CheckAv(sws_setColorspaceDetails(scaler_, coefficients,
        frame_->color_range == AVCOL_RANGE_JPEG, coefficients, 1, 0, 1 << 16, 1 << 16),
        "sws_setColorspaceDetails");
    QImage image(frame_->width, frame_->height, QImage::Format_RGB888);
    if (image.isNull()) throw std::bad_alloc();
    uint8_t* planes[] = {image.bits(), nullptr, nullptr, nullptr};
    const int strides[] = {image.bytesPerLine(), 0, 0, 0};
    CheckAv(sws_scale(scaler_, frame_->data, frame_->linesize, 0,
                      frame_->height, planes, strides), "sws_scale");
    const auto timestamp = frame_->best_effort_timestamp;
    av_frame_unref(frame_.get());
    // 图像直接拥有 RGB 内存，释放 AVFrame 不会破坏窗口排队的画面。
    picture(std::move(image), timestamp);
  }
}
}  // namespace lesson
