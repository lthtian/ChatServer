#ifndef VIDEO_PLAYER_VIDEO_DECODER_H_
#define VIDEO_PLAYER_VIDEO_DECODER_H_

#include <QImage>
#include <QObject>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

class VideoDecoder : public QObject {
  Q_OBJECT

 public:
  VideoDecoder();

 public slots:
  void Open(const QString& filename, quint64 session);
  void Read(quint64 session);

 signals:
  void Opened(quint64 session, qint64 duration_us);
  void FrameReady(quint64 session, QImage image, qint64 pts_us,
                  qint64 duration_us);
  void Ended(quint64 session);
  void Failed(quint64 session, QString message);

 private:
  QImage ConvertFrame();
  std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> input_;
  std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> context_;
  std::unique_ptr<AVPacket, void (*)(AVPacket*)> packet_;
  std::unique_ptr<AVFrame, void (*)(AVFrame*)> frame_;
  std::unique_ptr<AVFrame, void (*)(AVFrame*)> rgb_;
  std::unique_ptr<SwsContext, void (*)(SwsContext*)> scaler_;
  const AVStream* stream_ = nullptr;
  quint64 session_ = 0;
  qint64 fallback_duration_us_ = 40000;
  qint64 next_pts_us_ = 0;
  bool draining_ = false;
  bool has_frame_ = false;
};

#endif  // VIDEO_PLAYER_VIDEO_DECODER_H_
