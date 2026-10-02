#ifndef VIDEO_PLAYER_MEDIA_DECODER_H_
#define VIDEO_PLAYER_MEDIA_DECODER_H_

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QVector>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// 可跨线程传递的独立画面；时间均相对于同一个文件起点，单位微秒。
struct VideoPicture {
  QImage image;            // 拥有像素的 RGB 图像，不借用 AVFrame 内存。
  qint64 pts_us = 0;       // 画面的显示位置。
  qint64 duration_us = 0;  // 画面的预计持续时间。
};

// 设备格式的 PCM 块：S16、小端、左右声道交错。
struct AudioBlock {
  QByteArray pcm;     // 拥有采样字节，长度为每声道采样数 × 4。
  qint64 pts_us = 0;  // 第一个采样在公共媒体时间轴上的位置。
};

// 一次读包产生的结果；一个包可能没有输出，也可能产生多个帧。
struct MediaBatch {
  QVector<VideoPicture> video;  // 本次可用的视频画面。
  QVector<AudioBlock> audio;    // 本次可用的 PCM 块。
  bool eof = false;             // 两个解码器及重采样器均已排空。
};
Q_DECLARE_METATYPE(MediaBatch)

// 全部 FFmpeg 操作只在工作线程执行；不调用 QWidget 或音频设备。
class MediaDecoder : public QObject {
  Q_OBJECT
 public:
  MediaDecoder();            // 设置各资源的释放方式，尚未打开文件。
  ~MediaDecoder() override;  // 释放复制的声道布局，智能指针释放其他资源。

 public slots:
  void Open(QString filename, quint64 session);  // 打开文件并探测音视频轨道。
  void SetOutputRate(int rate);  // 设置设备选定的输出采样率，先于第一次 Read。
  void Read(
      quint64 session);  // 读取一个包并取完其输出，结果通过 BatchReady 返回。
  void Seek(qint64 position_us,
            quint64 session);  // 定位、清状态并切换会话编号。

 signals:
  void Opened(quint64 session, qint64 duration_us, bool has_audio,
              qint64 audio_end_us);  // 返回轨道信息；音轨结束位置未知时为 -1。
  void BatchReady(quint64 session, MediaBatch batch);  // 交付一次请求的结果。
  void Seeked(quint64 session);  // 定位完成，可以继续请求数据。
  void Failed(quint64 session, QString message);  // 返回可展示的错误说明。

 private:
  void OpenCodec(const AVStream* stream,
                 AVCodecContext** output);  // 建立解码实例。
  void Send(AVCodecContext* codec, const AVPacket* packet, bool video,
            MediaBatch& batch);  // 处理送包 EAGAIN，并接收全部可用输出。
  int Receive(AVCodecContext* codec, bool video,
              MediaBatch& batch);  // 接收至需输入或 EOF。
  void ConvertVideo(
      MediaBatch& batch);  // 转 RGB，并滤掉 seek 目标前的完整画面。
  void PrepareAudio();     // 按实际音频帧初始化或核对重采样器。
  int ConvertAudio(const AVFrame* source,
                   MediaBatch& batch);      // 转 PCM、标记时间及裁剪。
  void ResetDecodeState(qint64 target_us);  // 清理临时帧、重采样器和时间计数。

  std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)>
      input_;  // 容器与读包状态。
  std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)>
      video_;  // 视频解码实例。
  std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)>
      audio_;                                              // 可选音频解码实例。
  std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet_;  // 复用的输入包。
  std::unique_ptr<AVFrame, void (*)(AVFrame*)>
      frame_;  // 顺序接收两个解码器的结果。
  std::unique_ptr<AVFrame, void (*)(AVFrame*)> rgb_;  // 像素转换的目标帧。
  std::unique_ptr<SwsContext, void (*)(SwsContext*)>
      scaler_;  // 视频像素转换状态。
  std::unique_ptr<SwrContext, void (*)(SwrContext*)>
      resampler_;                           // 音频转换及延迟状态。
  const AVStream* video_stream_ = nullptr;  // 借用 input_ 的视频轨道。
  const AVStream* audio_stream_ = nullptr;  // 借用 input_ 的音频轨道，可为空。
  AVChannelLayout source_layout_{};  // 复制的输入声道布局，需单独 uninit。
  AVSampleFormat source_format_ = AV_SAMPLE_FMT_NONE;  // 当前输入采样格式。
  int source_rate_ = 0;                                // 当前输入采样率。
  int output_rate_ = 48000;   // Qt 设备支持的目标采样率。
  quint64 session_ = 0;       // 打开/跳转代次，供 GUI 丢弃过期结果。
  qint64 origin_us_ = 0;      // 所有轨道共同减去的容器起点。
  qint64 target_us_ = 0;      // 当前 seek 目标；更早的数据只用于恢复解码状态。
  qint64 next_video_us_ = 0;  // 视频时间戳缺失时的连续估算值。
  qint64 frame_duration_us_ = 40000;  // 视频时长缺失时按帧率估计。
  qint64 audio_origin_us_ = 0;        // 本次连续音频转换的首采样位置。
  qint64 audio_samples_ = 0;  // 已转换的每声道采样总数，用于避免累积舍入误差。
  bool audio_started_ = false;  // 是否已建立音频时间起点。
  bool eof_ = false;            // 是否已完成文件及转换器排空。
};
#endif  // VIDEO_PLAYER_MEDIA_DECODER_H_
