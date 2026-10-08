#pragma once

#include "media_io.h"

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace media {

// 一档输出的编码设置；CRF 控制质量，峰值码率和缓冲约束瞬时数据量。
struct Rendition {
  int height;  // 输出像素高度；此实验不放大小于目标高度的输入。
  int max_video_rate;  // H.264 VBV 最大码率，bit/s。
  int audio_rate;  // AAC 目标码率，bit/s；不是采样率。
  bool align_segments = false;  // HLS 在共同的两秒边界强制 IDR。
};

void TranscodeFile(const std::string& path, const std::string& destination,
                   const Rendition& rendition);  // 顺序处理一档完整音视频。

// 一条输入轨道对应一个解码实例和一个编码实例，输出容器由两条轨道共享。
class TrackTranscoder {
 public:
  TrackTranscoder(AVFormatContext* input, AVStream* stream, Output& output,
                  const Rendition& rendition);  // 配置编解码器及输出轨道。
  ~TrackTranscoder();  // 释放单独复制的声道布局。
  void Send(const AVPacket* packet);  // 提交输入包并取尽当前可解码的帧。
  void Finish();  // 依次排空解码、重采样/FIFO、编码；不写容器尾。

 private:
  void ConfigureVideo(AVFormatContext* input, const Rendition& rendition);
  // 配置 H.264、尺寸、像素格式、时间基、GOP 和质量选项。
  void ConfigureAudio(const Rendition& rendition);  // 配置 AAC 48 kHz stereo FLTP。
  int ReceiveFrames();  // 持续接收解码帧，按轨道类型转换后交给编码器。
  void ConvertVideo(const AVFrame* source);  // 缩放并换算显示时间戳。
  void ConvertAudio(const AVFrame* source);  // 初始化/校验重采样器和输入时间线。
  int Resample(const AVFrame* source);  // source=nullptr 表示排空滤波延迟。
  void EncodeAudio(bool final);  // 从 FIFO 拼出 AAC 所需的每声道采样数。
  void Encode(AVFrame* frame);  // 提交原始帧，nullptr 表示编码输入结束。
  int ReceivePackets();  // 取出编码包，交给共享的封装输出。

  AVStream* source_;  // 借用输入轨道，提供输入时间基和编码参数。
  Output& output_;  // 借用输出容器，负责音视频交错写盘。
  AVStream* target_ = nullptr;  // 借用输出轨道，保存编码器生成的参数。
  CodecPtr decoder_;  // 维护参考帧、解码延迟等输入状态。
  CodecPtr encoder_;  // 维护预测、码率控制和编码延迟等输出状态。
  FramePtr decoded_;  // 反复接收一帧解码结果。
  PacketPtr encoded_;  // 反复接收一个编码结果包。
  std::unique_ptr<SwsContext, void (*)(SwsContext*)> scaler_;  // 视频缩放/像素转换。
  std::unique_ptr<SwrContext, void (*)(SwrContext*)> resampler_;  // 音频转换和滤波延迟。
  std::unique_ptr<AVAudioFifo, void (*)(AVAudioFifo*)> fifo_;  // 按声道保存转换后的采样。
  AVChannelLayout input_layout_{};  // 第一帧的声道布局副本，校验后续帧。
  AVSampleFormat input_format_ = AV_SAMPLE_FMT_NONE;  // 第一帧的采样格式。
  int input_rate_ = 0;  // 输入采样率，不由 codec_id 推断。
  int64_t origin_us_ = 0;  // 两轨共同的容器起点，单位微秒。
  int64_t next_video_pts_ = 0;  // PTS 缺失时采用的预测值，单位为编码器 time_base。
  int64_t last_video_pts_ = AV_NOPTS_VALUE;  // 检测倒退/重复的视频 PTS。
  int64_t next_input_sample_ = AV_NOPTS_VALUE;  // 检测音频输入时间线空洞。
  int64_t next_audio_pts_ = 0;  // 下一编码音频帧的位置，单位为 1/48000 秒。
  int64_t frame_count_ = 0;  // 解码帧数，用于空流检查及运行结果。
  int64_t sample_count_ = 0;  // 送入音频编码器的每声道采样总数。
  int64_t packet_count_ = 0;  // 编码器产出的包数，包含排空阶段。
  bool align_segments_ = false;  // 是否按媒体时间强制分片关键帧。
  int64_t next_key_pts_ = AV_NOPTS_VALUE;  // 下一两秒边界，使用视频编码时间基。
};

}  // namespace media
