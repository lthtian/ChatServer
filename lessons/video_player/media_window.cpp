#include "media_window.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
#include <QStyleOptionSlider>
#include <QVBoxLayout>
#include <algorithm>
#include <stdexcept>

namespace {
QString TimeText(qint64 us) {
  const qint64 seconds = std::max<qint64>(0, us / 1000000);
  return QString("%1:%2")
      .arg(seconds / 60, 2, 10, QLatin1Char('0'))
      .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
}  // namespace

ProgressSlider::ProgressSlider(QWidget* parent)
    : QSlider(Qt::Horizontal, parent) {
  setRange(0, 10000);
}
void ProgressSlider::SetFromMouse(int x) {
  QStyleOptionSlider option;
  initStyleOption(&option);
  const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option,
                                               QStyle::SC_SliderGroove, this);
  const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option,
                                               QStyle::SC_SliderHandle, this);
  setValue(QStyle::sliderValueFromPosition(
      minimum(), maximum(), x - groove.x() - handle.width() / 2,
      std::max(1, groove.width() - handle.width()), option.upsideDown));
}
void ProgressSlider::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QSlider::mousePressEvent(event);
    return;
  }
  setSliderDown(true);
  SetFromMouse(event->x());
  event->accept();
}
void ProgressSlider::mouseMoveEvent(QMouseEvent* event) {
  if (isSliderDown())
    SetFromMouse(event->x());
  else
    QSlider::mouseMoveEvent(event);
}
void ProgressSlider::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && isSliderDown()) {
    SetFromMouse(event->x());
    setSliderDown(false);
    event->accept();
  } else
    QSlider::mouseReleaseEvent(event);
}

MediaWindow::MediaWindow()
    : view_(new VideoView(this)),
      play_button_(new QPushButton(this)),
      stop_button_(new QPushButton(QString::fromUtf8("停止"), this)),
      mute_button_(new QPushButton(QString::fromUtf8("静音"), this)),
      progress_(new ProgressSlider(this)),
      volume_(new QSlider(Qt::Horizontal, this)),
      status_(new QLabel(this)),
      decoder_(new MediaDecoder) {
  qRegisterMetaType<MediaBatch>();
  setWindowTitle(QString::fromUtf8("音视频播放器"));
  resize(960, 640);
  setMinimumSize(600, 360);
  CreateControls();
  ConnectDecoder();
  timer_.setInterval(10);
  timer_.setTimerType(Qt::PreciseTimer);
  connect(&timer_, &QTimer::timeout, this, &MediaWindow::Tick);
  thread_.start();
  UpdateControls();
}
MediaWindow::~MediaWindow() {
  audio_.Close();
  thread_.requestInterruption();
  thread_.quit();
  thread_.wait();
}

void MediaWindow::CreateControls() {
  auto* open = new QPushButton(QString::fromUtf8("打开视频"), this);
  auto* fullscreen = new QPushButton(QString::fromUtf8("全屏"), this);
  play_button_->setObjectName("mediaPlayButton");
  progress_->setObjectName("mediaProgress");
  mute_button_->setCheckable(true);
  volume_->setRange(0, 100);
  volume_->setValue(70);
  volume_->setMaximumWidth(120);
  volume_->setToolTip(QString::fromUtf8("音量"));
  auto* controls = new QHBoxLayout;
  for (auto* button : {open, play_button_, stop_button_, mute_button_})
    controls->addWidget(button);
  controls->addWidget(volume_);
  controls->addStretch();
  controls->addWidget(fullscreen);
  auto* layout = new QVBoxLayout(this);
  layout->addWidget(view_, 1);
  layout->addWidget(progress_);
  layout->addWidget(status_);
  layout->addLayout(controls);
  connect(open, &QPushButton::clicked, this, [this] {
    const QString file = QFileDialog::getOpenFileName(
        this, QString::fromUtf8("打开本地视频"), {},
        QString::fromUtf8(
            "视频 (*.mp4 *.mkv *.mov *.avi *.webm);;所有文件 (*)"));
    if (!file.isEmpty()) OpenFile(file);
  });
  connect(play_button_, &QPushButton::clicked, this,
          &MediaWindow::TogglePlayback);
  connect(stop_button_, &QPushButton::clicked, this, &MediaWindow::Stop);
  connect(fullscreen, &QPushButton::clicked, this,
          &MediaWindow::ToggleFullscreen);
  connect(volume_, &QSlider::valueChanged, this, [this](int value) {
    audio_.SetVolume(value, mute_button_->isChecked());
  });
  connect(mute_button_, &QPushButton::toggled, this,
          [this](bool muted) { audio_.SetVolume(volume_->value(), muted); });
  // 拖动期间冻结播放；松手只定位一次，避免每移动一个像素就清空解码器。
  connect(progress_, &QSlider::sliderPressed, this, [this] {
    resume_after_drag_ = state_ == State::Playing ||
                         (state_ == State::Buffering && play_after_buffering_);
    if (state_ == State::Playing) TogglePlayback();
  });
  connect(progress_, &QSlider::valueChanged, this, [this](int value) {
    if (progress_->isSliderDown())
      status_->setText(QString::fromUtf8("定位到 ") +
                       TimeText(duration_us_ * value / 10000));
  });
  connect(progress_, &QSlider::sliderReleased, this, [this] {
    StartSeek(duration_us_ * progress_->value() / 10000, resume_after_drag_);
  });
  const auto shortcut = [this](const QKeySequence& key, auto action) {
    auto* item = new QShortcut(key, this);
    connect(item, &QShortcut::activated, this, action);
  };
  shortcut(QKeySequence(Qt::Key_Space), [this] { TogglePlayback(); });
  shortcut(QKeySequence(Qt::Key_Left),
           [this] { SeekTo(PositionUs() - 5000000); });
  shortcut(QKeySequence(Qt::Key_Right),
           [this] { SeekTo(PositionUs() + 5000000); });
  shortcut(QKeySequence(Qt::Key_F), [this] { ToggleFullscreen(); });
  shortcut(QKeySequence(Qt::Key_Escape), [this] {
    if (isFullScreen()) showNormal();
  });
}

void MediaWindow::ConnectDecoder() {
  decoder_->moveToThread(&thread_);
  connect(&thread_, &QThread::finished, decoder_, &QObject::deleteLater);
  // 显式排队连接保证槽函数在 decoder_ 所属线程执行。
  connect(this, &MediaWindow::OpenRequested, decoder_, &MediaDecoder::Open,
          Qt::QueuedConnection);
  connect(this, &MediaWindow::OutputRateRequested, decoder_,
          &MediaDecoder::SetOutputRate, Qt::QueuedConnection);
  connect(this, &MediaWindow::ReadRequested, decoder_, &MediaDecoder::Read,
          Qt::QueuedConnection);
  connect(this, &MediaWindow::SeekRequested, decoder_, &MediaDecoder::Seek,
          Qt::QueuedConnection);
  connect(decoder_, &MediaDecoder::Opened, this,
          [this](quint64 session, qint64 duration, bool has_audio,
                 qint64 audio_end) {
            if (session != session_) return;
            try {
              opened_ = true;
              duration_us_ = duration;
              audio_end_us_ = audio_end;
              if (has_audio) {
                emit OutputRateRequested(audio_.Open());
                audio_.Restart(0);
                status_->setToolTip(audio_.Description());
              } else
                status_->setToolTip(
                    QString::fromUtf8("此文件没有音轨，使用单调时钟"));
              RequestData();
            } catch (const std::exception& e) {
              Fail(QString::fromUtf8(e.what()));
            }
          });
  connect(decoder_, &MediaDecoder::Seeked, this, [this](quint64 session) {
    if (session != session_) return;
    in_flight_ = false;
    RequestData();
  });
  connect(decoder_, &MediaDecoder::BatchReady, this,
          [this](quint64 session, MediaBatch batch) {
            // 排队信号已经发出后无法撤回；代次检查避免 seek
            // 后出现旧声音和旧画面。
            if (session != session_) return;
            in_flight_ = false;
            try {
              AcceptBatch(std::move(batch));
              TryStart();
              Tick();
              RequestData();
            } catch (const std::exception& e) {
              Fail(QString::fromUtf8(e.what()));
            }
          });
  connect(decoder_, &MediaDecoder::Failed, this,
          [this](quint64 session, QString message) {
            if (session == session_) Fail(message);
          });
}

void MediaWindow::ClearPending(qint64 position_us) {
  pictures_.clear();
  picture_bytes_ = 0;
  in_flight_ = eof_ = false;
  base_us_ = last_video_end_us_ = position_us;
  audio_clock_ = false;
  clock_.invalidate();
}
void MediaWindow::OpenFile(const QString& filename) {
  ++session_;
  timer_.stop();
  audio_.Close();
  ClearPending(0);
  filename_ = filename;
  error_.clear();
  opened_ = false;
  duration_us_ = 0;
  audio_end_us_ = -1;
  state_ = State::Buffering;
  play_after_buffering_ = true;
  view_->SetImage({});
  setWindowTitle(QFileInfo(filename).fileName() +
                 QString::fromUtf8(" · 音视频播放器"));
  UpdateControls();
  emit OpenRequested(filename, session_);
}

void MediaWindow::AcceptBatch(MediaBatch batch) {
  for (auto& picture : batch.video) {
    picture_bytes_ += picture.image.sizeInBytes();
    pictures_.push_back(std::move(picture));
  }
  if (picture_bytes_ > 64 * 1024 * 1024 || pictures_.size() > 180)
    throw std::runtime_error("声画交错间隔过大，画面缓存达到本例上限");
  for (auto& block : batch.audio) audio_.Append(std::move(block));
  eof_ = batch.eof;
}
void MediaWindow::RequestData() {
  if (!opened_ || in_flight_ || eof_ ||
      (state_ != State::Buffering && state_ != State::Playing))
    return;
  const qint64 position = PositionUs();
  const bool need_video =
      pictures_.empty() || pictures_.back().pts_us < position + 150000;
  const bool need_audio =
      audio_.IsOpen() && audio_.BufferedEndUs() < position + 150000 &&
      (audio_end_us_ < 0 || audio_.BufferedEndUs() + 1000 < audio_end_us_);
  if (state_ == State::Buffering || need_video || need_audio) {
    in_flight_ = true;
    emit ReadRequested(session_);
  }
}
void MediaWindow::TryStart() {
  if (state_ != State::Buffering) return;
  if (pictures_.empty()) {
    if (eof_) throw std::runtime_error("定位后没有可显示的画面");
    return;
  }
  const bool audio_ready =
      !audio_.IsOpen() || eof_ || audio_.BufferedEndUs() >= base_us_ + 100000 ||
      (audio_end_us_ >= 0 && audio_.BufferedEndUs() + 1000 >= audio_end_us_);
  if (!audio_ready) return;
  audio_clock_ =
      audio_.IsOpen() && (audio_end_us_ < 0 || base_us_ < audio_end_us_);
  state_ = play_after_buffering_ ? State::Playing : State::Paused;
  if (play_after_buffering_) {
    clock_.start();
    timer_.start();
  } else {
    // 暂停 seek 只显示目标预览，不向设备写 PCM；已有 PCM 留给恢复播放。
    auto picture = std::move(pictures_.front());
    pictures_.pop_front();
    picture_bytes_ -= picture.image.sizeInBytes();
    view_->SetImage(std::move(picture.image));
    last_video_end_us_ = picture.pts_us + picture.duration_us;
    emit FramePresented(picture.pts_us);
    audio_.Pause();
  }
  UpdateControls();
}
qint64 MediaWindow::PositionUs() const {
  if (state_ != State::Playing) return base_us_;
  if (audio_clock_) return audio_.PositionUs();
  return base_us_ + (clock_.isValid() ? clock_.nsecsElapsed() / 1000 : 0);
}

void MediaWindow::Tick() {
  if (state_ != State::Playing) return;
  try {
    const bool audio_finished =
        eof_ ||
        (audio_end_us_ >= 0 && audio_.BufferedEndUs() + 1000 >= audio_end_us_);
    audio_.Pump(audio_finished);
    if (audio_clock_ && audio_finished && audio_.Drained()) {
      // 音轨先结束时，以同一个位置接续单调时钟，让剩余视频继续显示。
      base_us_ = audio_.PositionUs();
      audio_clock_ = false;
      clock_.restart();
    }
    const qint64 position = PositionUs();
    while (!pictures_.empty() && pictures_.front().pts_us <= position) {
      auto picture = std::move(pictures_.front());
      pictures_.pop_front();
      picture_bytes_ -= picture.image.sizeInBytes();
      last_video_end_us_ = picture.pts_us + picture.duration_us;
      // 若另一张图也已到显示时间，跳过这张迟到图；只绘制最后一张到期画面。
      if (!pictures_.empty() && pictures_.front().pts_us <= position) continue;
      view_->SetImage(std::move(picture.image));
      emit FramePresented(picture.pts_us);
    }
    if (eof_ && pictures_.empty() && position >= last_video_end_us_ &&
        audio_.Drained()) {
      base_us_ = std::max(last_video_end_us_,
                          audio_.IsOpen() ? audio_.BufferedEndUs() : 0);
      state_ = State::Ended;
      timer_.stop();
    }
    RequestData();
    UpdateControls();
  } catch (const std::exception& e) {
    Fail(QString::fromUtf8(e.what()));
  }
}

void MediaWindow::SeekTo(qint64 position_us) {
  StartSeek(position_us,
            state_ == State::Playing ||
                (state_ == State::Buffering && play_after_buffering_));
}
void MediaWindow::StartSeek(qint64 position_us, bool play_after) {
  if (!opened_ || state_ == State::Error) return;
  const qint64 target = std::clamp<qint64>(
      position_us, 0, std::max<qint64>(0, duration_us_ - 1000));
  try {
    ++session_;
    timer_.stop();
    ClearPending(target);
    audio_.Restart(target);
    state_ = State::Buffering;
    play_after_buffering_ = play_after;
    // 定位也是在途操作，等 Seeked 返回后再发读包请求。
    in_flight_ = true;
    UpdateControls();
    emit SeekRequested(target, session_);
  } catch (const std::exception& e) {
    Fail(QString::fromUtf8(e.what()));
  }
}
void MediaWindow::TogglePlayback() {
  if (state_ == State::Playing) {
    base_us_ = PositionUs();
    audio_.Pause();
    state_ = State::Paused;
    timer_.stop();
  } else if (state_ == State::Paused) {
    audio_.Resume();
    state_ = State::Playing;
    clock_.restart();
    timer_.start();
    Tick();
  } else if (state_ == State::Stopped || state_ == State::Ended)
    StartSeek(0, true);
  else if (state_ == State::Buffering)
    play_after_buffering_ = !play_after_buffering_;
  UpdateControls();
}
void MediaWindow::Stop() {
  if (!opened_) return;
  try {
    ++session_;
    timer_.stop();
    ClearPending(0);
    audio_.Restart(0);
    audio_.Pause();
    state_ = State::Stopped;
    view_->SetImage({});
    UpdateControls();
  } catch (const std::exception& e) {
    Fail(QString::fromUtf8(e.what()));
  }
}
void MediaWindow::Fail(const QString& message) {
  base_us_ = PositionUs();
  ++session_;
  state_ = State::Error;
  error_ = message;
  timer_.stop();
  audio_.Close();
  pictures_.clear();
  picture_bytes_ = 0;
  UpdateControls();
}
void MediaWindow::ToggleFullscreen() {
  if (isFullScreen())
    showNormal();
  else
    showFullScreen();
}
void MediaWindow::UpdateControls() {
  play_button_->setEnabled(state_ != State::Empty && state_ != State::Error);
  stop_button_->setEnabled(opened_ && state_ != State::Error);
  progress_->setEnabled(opened_ && duration_us_ > 0 && state_ != State::Error);
  const bool playing = state_ == State::Playing ||
                       (state_ == State::Buffering && play_after_buffering_);
  play_button_->setText(playing ? QString::fromUtf8("暂停")
                                : QString::fromUtf8("播放"));
  if (state_ == State::Error) {
    status_->setText(QString::fromUtf8("无法播放：") + error_);
    return;
  }
  const QStringList names{
      QString::fromUtf8("请选择视频"), QString::fromUtf8("准备数据"),
      QString::fromUtf8("播放中"),     QString::fromUtf8("已暂停"),
      QString::fromUtf8("已停止"),     QString::fromUtf8("播放结束"),
      QString::fromUtf8("错误")};
  if (!progress_->isSliderDown()) {
    const qint64 position = PositionUs();
    progress_->setValue(duration_us_ > 0
                            ? static_cast<int>(std::clamp<qint64>(
                                  position * 10000 / duration_us_, 0, 10000))
                            : 0);
    status_->setText(names[static_cast<int>(state_)] + " · " +
                     TimeText(position) + " / " + TimeText(duration_us_));
  }
}
