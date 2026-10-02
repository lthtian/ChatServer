#ifndef VIDEO_PLAYER_PLAYER_WINDOW_H_
#define VIDEO_PLAYER_PLAYER_WINDOW_H_

#include <QElapsedTimer>
#include <QImage>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include "video_view.h"

class QLabel;
class QPushButton;
class VideoDecoder;

class PlayerWindow : public QWidget {
  Q_OBJECT

 public:
  enum class State { Empty, Loading, Playing, Paused, Ended, Error };
  Q_ENUM(State)
  PlayerWindow();
  ~PlayerWindow() override;
  void OpenFile(const QString& filename);
  State state() const { return state_; }
  qint64 positionUs() const;
  QImage CurrentImage() const { return view_->image(); }
  QString errorText() const { return error_; }

 signals:
  void OpenRequested(QString filename, quint64 session);
  void FrameRequested(quint64 session);
  void FramePresented(qint64 pts_us);

 private:
  void TogglePlayback();
  void Tick();
  void UpdateControls();
  VideoView* view_;
  QPushButton* play_button_;
  QLabel* status_;
  VideoDecoder* decoder_;
  QThread thread_;
  QTimer timer_;
  QElapsedTimer clock_;
  State state_ = State::Empty;
  QString filename_;
  QString error_;
  quint64 session_ = 0;
  QImage pending_image_;
  qint64 pending_pts_us_ = 0;
  qint64 pending_duration_us_ = 0;
  qint64 first_pts_us_ = 0;
  qint64 paused_position_us_ = 0;
  qint64 duration_us_ = 0;
  qint64 last_end_us_ = 0;
  bool exhausted_ = false;
};

#endif  // VIDEO_PLAYER_PLAYER_WINDOW_H_
