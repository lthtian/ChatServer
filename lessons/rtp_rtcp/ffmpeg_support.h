#pragma once

#include <memory>
#include <stdexcept>
#include <string>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
}

namespace lesson {
// EOF/EAGAIN 由调用处处理，其余失败转为带 API 名称的异常。
inline void CheckAv(int result, const char* operation) {
  if (result >= 0) return;
  char message[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(result, message, sizeof(message));
  throw std::runtime_error(std::string(operation) + ": " + message);
}
// 这些 deleter 对应 FFmpeg 的所有权规则，unique_ptr 负责异常路径上的释放。
inline void CloseInput(AVFormatContext* p) { avformat_close_input(&p); }
inline void FreeFilter(AVBSFContext* p) { av_bsf_free(&p); }
inline void FreeCodec(AVCodecContext* p) { avcodec_free_context(&p); }
inline void FreePacket(AVPacket* p) { av_packet_free(&p); }
inline void FreeFrame(AVFrame* p) { av_frame_free(&p); }
using InputPtr = std::unique_ptr<AVFormatContext, decltype(&CloseInput)>;
using FilterPtr = std::unique_ptr<AVBSFContext, decltype(&FreeFilter)>;
using CodecPtr = std::unique_ptr<AVCodecContext, decltype(&FreeCodec)>;
using PacketPtr = std::unique_ptr<AVPacket, decltype(&FreePacket)>;
using FramePtr = std::unique_ptr<AVFrame, decltype(&FreeFrame)>;
}  // namespace lesson
