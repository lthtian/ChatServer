#pragma once

#include <QByteArray>
#include <QImage>
#include <functional>
#include "ffmpeg_support.h"

namespace lesson {
class VideoDecoder {
 public:
  using Picture = std::function<void(QImage, qint64)>;
  VideoDecoder();   // 建立 H.264 软件解码器，包/帧时间戳统一使用 1/90000 秒。
  ~VideoDecoder();  // 释放像素转换器，其余对象由智能指针释放。
  // 输入完整 Annex B 访问单元；回调交出拥有像素的图像和展开后的显示时间戳。
  void Decode(const QByteArray& bytes, qint64 timestamp, const Picture& picture);
  void Reset();     // 丢包后清除解码器参考状态，重组层负责等待 IDR。
 private:
  // 持续取输出直至 EAGAIN；一个输入包不等于恰好一张输出图像。
  void Receive(const Picture& picture);
  CodecPtr codec_{nullptr, FreeCodec};     // 本场解码工作实例及参考图像状态。
  PacketPtr packet_{av_packet_alloc(), FreePacket}; // 拷贝完整访问单元的输入对象。
  FramePtr frame_{av_frame_alloc(), FreeFrame};     // 可重复接收的解码帧对象。
  SwsContext* scaler_ = nullptr;          // YUV/其他像素格式到 RGB 的转换状态。
};
}  // namespace lesson
