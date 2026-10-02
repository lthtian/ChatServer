#include "player_window.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

#include "video_decoder.h"

PlayerWindow::PlayerWindow()
    : view_(new VideoView(this)),
      play_button_(new QPushButton(this)),
      status_(new QLabel(this)),
      decoder_(new VideoDecoder) {
  setWindowTitle(QString::fromUtf8("无声视频播放器"));
  resize(960, 640);
  setMinimumSize(480, 320);
  auto* open = new QPushButton(QString::fromUtf8("打开视频"), this);
  play_button_->setObjectName("playButton");
  status_->setWordWrap(true);
  auto* controls = new QHBoxLayout;
  controls->addWidget(open);
  controls->addWidget(play_button_);
  controls->addWidget(status_, 1);
  auto* layout = new QVBoxLayout(this);
  layout->addWidget(view_, 1);
  layout->addLayout(controls);
  // 点击打开按钮后，在 GUI 线程选择文件；选定后由 OpenFile 发出打开请求。
  connect(open, &QPushButton::clicked, this, [this] {
    const QString file = QFileDialog::getOpenFileName(
        this, QString::fromUtf8("打开本地视频"), {},
        QString::fromUtf8(
            "视频 (*.mp4 *.mkv *.mov *.avi *.webm);;所有文件 (*)"));
    if (!file.isEmpty()) OpenFile(file);
  });
  // 点击播放按钮，根据当前状态执行暂停、继续播放或从头重播。
  connect(play_button_, &QPushButton::clicked, this,
          &PlayerWindow::TogglePlayback);

  decoder_->moveToThread(&thread_);
  // 工作线程结束时延迟销毁 decoder_，由其成员的删除器释放 FFmpeg 资源。
  connect(&thread_, &QThread::finished, decoder_, &QObject::deleteLater);
  // 将打开请求排入解码线程，在该线程执行 Open，完成文件探测和解码器初始化。
  connect(this, &PlayerWindow::OpenRequested, decoder_, &VideoDecoder::Open,
          Qt::QueuedConnection);
  // 将取帧请求排入解码线程；Read 每次最多返回一帧，期间可能读取多个包。
  connect(this, &PlayerWindow::FrameRequested, decoder_, &VideoDecoder::Read,
          Qt::QueuedConnection);
  // GUI 收到打开成功通知后，保存媒体时长并请求首帧；此时尚未启动播放时钟。
  connect(decoder_, &VideoDecoder::Opened, this,
          [this](quint64 session, qint64 duration) {
            if (session != session_) return;
            duration_us_ = duration;
            emit FrameRequested(session_);
          });
  // GUI 收到帧后，首帧启动播放时钟；图像暂存为待显示帧，由 Tick 按 PTS 调度。
  connect(decoder_, &VideoDecoder::FrameReady, this,
          [this](quint64 session, QImage image, qint64 pts, qint64 duration) {
            // 切换文件后，上一文件已经排入事件队列的结果不能污染当前画面。
            if (session != session_) return;
            if (state_ == State::Loading) {
              first_pts_us_ = pts;
              clock_.start();
              state_ = State::Playing;
              timer_.start();
            }
            pending_image_ = std::move(image);
            pending_pts_us_ = std::max<qint64>(0, pts - first_pts_us_);
            pending_duration_us_ = duration;
            Tick();
          });
  // 解码结果已取完；Tick 等最后一帧的显示时长结束后，再标记播放结束。
  connect(decoder_, &VideoDecoder::Ended, this, [this](quint64 session) {
    if (session != session_) return;
    exhausted_ = true;
    Tick();
  });
  // GUI 收到错误后，保存播放位置、停止定时器、清除待显示帧并更新错误提示。
  connect(decoder_, &VideoDecoder::Failed, this,
          [this](quint64 session, QString message) {
            if (session != session_) return;
            paused_position_us_ = positionUs();
            state_ = State::Error;
            error_ = std::move(message);
            pending_image_ = {};
            timer_.stop();
            UpdateControls();
          });
  // 定时器只负责唤醒；实际播放位置由单调时钟计算，避免累积定时误差。
  timer_.setInterval(10);
  timer_.setTimerType(Qt::PreciseTimer);
  // GUI 定时检查待显示帧是否到达目标时间；一次 timeout 不一定会换图。
  connect(&timer_, &QTimer::timeout, this, &PlayerWindow::Tick);
  thread_.start();
  UpdateControls();
}

PlayerWindow::~PlayerWindow() {
  thread_.requestInterruption();
  thread_.quit();
  thread_.wait();
}

void PlayerWindow::OpenFile(const QString& filename) {
  ++session_;
  filename_ = filename;
  timer_.stop();
  state_ = State::Loading;
  error_.clear();
  clock_.invalidate();
  pending_image_ = {};
  paused_position_us_ = 0;
  duration_us_ = 0;
  last_end_us_ = 0;
  exhausted_ = false;
  view_->SetImage({});
  UpdateControls();
  emit OpenRequested(filename_, session_);
}

qint64 PlayerWindow::positionUs() const {
  return paused_position_us_ + (state_ == State::Playing && clock_.isValid()
                                    ? clock_.nsecsElapsed() / 1000
                                    : 0);
}

void PlayerWindow::TogglePlayback() {
  if (state_ == State::Ended) {
    OpenFile(filename_);
  } else if (state_ == State::Playing) {
    paused_position_us_ = positionUs();
    state_ = State::Paused;
    timer_.stop();
  } else if (state_ == State::Paused) {
    clock_.restart();
    state_ = State::Playing;
    timer_.start();
  }
  UpdateControls();
}

void PlayerWindow::Tick() {
  if (state_ != State::Playing) return;
  const qint64 position = positionUs();
  if (!pending_image_.isNull() && pending_pts_us_ <= position) {
    view_->SetImage(std::move(pending_image_));
    pending_image_ = {};
    last_end_us_ = pending_pts_us_ + pending_duration_us_;
    emit FramePresented(pending_pts_us_);
    // 显示一帧后才请求下一帧：最多一个预备帧，不让排队图像随片长增长。
    emit FrameRequested(session_);
  }
  if (exhausted_ && pending_image_.isNull() && position >= last_end_us_) {
    paused_position_us_ = last_end_us_;
    state_ = State::Ended;
    timer_.stop();
  }
  UpdateControls();
}

void PlayerWindow::UpdateControls() {
  const bool usable = state_ == State::Playing || state_ == State::Paused ||
                      state_ == State::Ended;
  play_button_->setEnabled(usable);
  play_button_->setText(state_ == State::Playing ? QString::fromUtf8("暂停")
                        : state_ == State::Ended ? QString::fromUtf8("重新播放")
                                                 : QString::fromUtf8("播放"));
  if (state_ == State::Error) {
    status_->setText(QString::fromUtf8("无法播放：") + error_);
    return;
  }
  const QString names[] = {
      QString::fromUtf8("请选择视频"), QString::fromUtf8("正在打开"),
      QString::fromUtf8("播放中"),     QString::fromUtf8("已暂停"),
      QString::fromUtf8("播放结束"),   QString::fromUtf8("无法播放")};
  const QString duration =
      duration_us_ > 0 ? QString::number(duration_us_ / 1000000.0, 'f', 2)
                       : QString::fromUtf8("未知");
  status_->setText(names[static_cast<int>(state_)] +
                   QString::fromUtf8(" · %1 / %2 秒")
                       .arg(positionUs() / 1000000.0, 0, 'f', 2)
                       .arg(duration));
}
