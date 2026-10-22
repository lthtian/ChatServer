#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QLabel>
#include <QNetworkDatagram>
#include <QPixmap>
#include <QRandomGenerator>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>
#include <deque>
#include <iostream>
#include <optional>
#include "rtcp.h"
#include "video_decoder.h"
#include "video_view.h"
#include "wire.h"

namespace {
using namespace lesson;

// 一个窗口即一个单视频源接收会话；网络、重排与轻量软件解码在 GUI 事件循环运行。
class ReceiverWindow : public QWidget {
 public:
  ReceiverWindow(const QHostAddress& bind, quint16 port, QString first_frame, QString screenshot)
      : first_frame_(std::move(first_frame)), screenshot_(std::move(screenshot)) {
    setWindowTitle(QString::fromUtf8("RTP / RTCP 视频接收实验"));
    resize(900, 600);
    auto* layout = new QVBoxLayout(this);
    view_ = new VideoView(this);
    status_ = new QLabel(QString::fromUtf8("等待 RTP 视频；先启动接收端，再启动发送端。"), this);
    status_->setWordWrap(true);
    layout->addWidget(view_, 1);
    layout->addWidget(status_);
    if (!rtp_.bind(bind, port, QUdpSocket::DontShareAddress) ||
        !rtcp_.bind(bind, quint16(port + 1), QUdpSocket::DontShareAddress))
      throw std::runtime_error("cannot bind receiver RTP/RTCP ports");
    // readyRead 读取 UDP 报文；真正的顺序整理在 ReorderBuffer 内，而不是 socket 内。
    connect(&rtp_, &QUdpSocket::readyRead, this, [this] { Guard([this] { ReadRtp(); }); });
    connect(&rtcp_, &QUdpSocket::readyRead, this, [this] { Guard([this] { ReadRtcp(); }); });
    connect(&tick_, &QTimer::timeout, this, [this] { Guard([this] { Tick(); }); });
    connect(&reports_, &QTimer::timeout, this, [this] { Guard([this] { Report(); }); });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] { Summary(); });
    clock_.start();
    tick_.setTimerType(Qt::PreciseTimer);
    tick_.start(10);
    reports_.start(1000);
    std::cout << "receiver listening=" << bind.toString().toStdString() << ':' << port
              << " rtcp=" << port + 1 << std::endl;
  }

 private:
  struct Picture {
    QImage image;       // 拥有 RGB 像素的图像。
    qint64 timestamp;   // 已展开的 90 kHz 显示时间戳。
  };

  // 所有网络/解码错误在边界记录；退出时由栈对象释放 socket、定时器与 FFmpeg 状态。
  void Guard(const std::function<void()>& action) {
    try { action(); }
    catch (const std::exception& error) {
      std::cerr << "receiver failed: " << error.what() << std::endl;
      tick_.stop(); reports_.stop(); QCoreApplication::exit(1);
    }
  }

  void ReadRtp() {
    // 一轮最多处理 128 个报文，把重绘/定时器的执行机会还给 Qt。
    for (int i = 0; i < 128 && rtp_.hasPendingDatagrams(); ++i) {
      if (rtp_.pendingDatagramSize() > 2048) { rtp_.receiveDatagram(1); ++invalid_; continue; }
      const auto datagram = rtp_.receiveDatagram(2048);
      RtpPacket packet;
      if (!ParseRtp(datagram.data(), packet) || packet.payload_type != 96) { ++invalid_; continue; }
      if (!stats_.ready()) {
        peer_ = datagram.senderAddress();
        peer_rtp_port_ = datagram.senderPort();
        source_ = packet.ssrc;
      }
      if (datagram.senderAddress() != peer_ || datagram.senderPort() != peer_rtp_port_ ||
          packet.ssrc != source_) { ++invalid_; continue; }
      const qint64 now = clock_.nsecsElapsed() / 1000;
      if (!stats_.Observe(packet, now)) { ++invalid_; continue; }
      // SR 可能先于首个 RTP 到达。先暂存，只有媒体地址和 SSRC 匹配后才绑定它。
      if (early_sr_) {
        if (early_sr_peer_ == peer_ && early_sr_->ssrc == source_ && now - early_sr_time_ < 2000000)
          AcceptSr(*early_sr_, early_sr_port_, early_sr_time_);
        early_sr_.reset();
      }
      if (stats_.received() <= 6)
        std::cout << "RTP recv seq=" << packet.sequence << " ts=" << packet.timestamp
                  << " marker=" << packet.marker << " nal_type=" << (quint8(packet.payload[0]) & 31)
                  << " bytes=" << datagram.data().size() << std::endl;
      reorder_.Insert(std::move(packet), now);
      Drain(now);
    }
    if (rtp_.hasPendingDatagrams())
      QTimer::singleShot(0, this, [this] { Guard([this] { ReadRtp(); }); });
  }

  void Drain(qint64 now) {
    reorder_.Drain(now, [this](const RtpPacket& packet) {
      assembler_.Push(packet, [this](QByteArray bytes, quint32 timestamp) {
        // 线上的 32 位时间戳会回绕；解码器使用连续的 64 位时间戳。
        if (!have_timestamp_) { timestamp_ = timestamp; have_timestamp_ = true; }
        timestamp_ = Unwrap(timestamp, timestamp_, 32);
        decoder_.Decode(bytes, timestamp_, [this](QImage image, qint64 pts) { Queue(std::move(image), pts); });
      });
    }, [this](qint64 missing) {
      gaps_ += quint64(missing);
      assembler_.Lost();
      decoder_.Reset();
      waiting_recovery_ = true;
      std::cout << "RTP gap packets=" << missing << "; discard damaged unit, wait for IDR" << std::endl;
    });
  }

  void Queue(QImage image, qint64 timestamp) {
    ++decoded_;
    if (waiting_recovery_) {
      waiting_recovery_ = false;
      std::cout << "RTP recovered at IDR pts_ticks=" << timestamp << std::endl;
    }
    if (decoded_ == 1) {
      first_timestamp_ = timestamp;
      play_start_us_ = clock_.nsecsElapsed() / 1000 + 100000;
      if (!first_frame_.isEmpty() && !image.save(first_frame_))
        throw std::runtime_error("cannot save first decoded frame");
    }
    // 固定 100ms 起播余量用于观察；队列同时受张数和实际 RGB 字节数约束。
    if (image.sizeInBytes() > 32 * 1024 * 1024) throw std::runtime_error("picture exceeds 32 MiB");
    while (!pictures_.empty() &&
           (pictures_.size() >= 32 || picture_bytes_ + image.sizeInBytes() > 32 * 1024 * 1024)) {
      picture_bytes_ -= pictures_.front().image.sizeInBytes();
      pictures_.pop_front(); ++display_dropped_;
    }
    picture_bytes_ += image.sizeInBytes();
    pictures_.push_back({std::move(image), timestamp});
  }

  void Tick() {
    const qint64 now = clock_.nsecsElapsed() / 1000;
    Drain(now);  // 即使下一包没有到来，也要让缺包等待在超时后结束。
    QImage latest;
    qint64 timestamp = 0;
    while (!pictures_.empty()) {
      const qint64 due = play_start_us_ + (pictures_.front().timestamp - first_timestamp_) * 1000000 / kClockRate;
      if (now < due) break;
      if (!latest.isNull()) ++display_dropped_;
      timestamp = pictures_.front().timestamp;
      latest = std::move(pictures_.front().image);
      picture_bytes_ -= latest.sizeInBytes();
      pictures_.pop_front();
    }
    if (!latest.isNull()) {
      view_->SetImage(std::move(latest));
      ++presented_;
      if (presented_ <= 3)
        std::cout << "picture presented=" << presented_ << " pts_ticks=" << timestamp
                  << " relative_ms=" << (timestamp - first_timestamp_) * 1000 / kClockRate << std::endl;
      if (presented_ == 1 && !screenshot_.isEmpty()) {
        QTimer::singleShot(150, this, [this] {
          Guard([this] {
            if (!grab().save(screenshot_)) throw std::runtime_error("cannot save receiver screenshot");
          });
        });
      }
    }
    if (now - last_status_us_ >= 100000) {
      last_status_us_ = now;
      status_->setText(QString::fromUtf8("%1 | 收到 %2 包 · 解码 %3 帧 · 显示 %4 帧\n"
          "缺包跳过 %5 · 过晚/重复 %6 · 重组丢弃 %7 · 等待队列 %8 包")
          .arg(ended_ ? QString::fromUtf8("发送结束") : QString::fromUtf8("RTP 接收中"))
          .arg(stats_.received()).arg(decoded_).arg(presented_).arg(gaps_)
          .arg(reorder_.rejected()).arg(assembler_.dropped()).arg(reorder_.size()));
    }
  }

  void ReadRtcp() {
    while (rtcp_.hasPendingDatagrams()) {
      if (rtcp_.pendingDatagramSize() > 2048) { rtcp_.receiveDatagram(1); ++invalid_; continue; }
      const auto datagram = rtcp_.receiveDatagram(2048);
      if (stats_.ready() && datagram.senderAddress() != peer_) continue;
      QVector<RtcpEvent> events;
      if (!ParseRtcp(datagram.data(), events)) { ++invalid_; continue; }
      for (const auto& event : events) {
        if (!stats_.ready()) {
          if (event.type == 200) {
            early_sr_ = event.sender;
            early_sr_peer_ = datagram.senderAddress();
            early_sr_port_ = datagram.senderPort();
            early_sr_time_ = clock_.nsecsElapsed() / 1000;
          }
          continue;
        }
        if (event.ssrc != source_) continue;
        if (event.type == 200) {
          // 从真实 SR 的来源取得 RTCP 返回端口，不假定所有发送程序都绑定 5007。
          AcceptSr(event.sender, datagram.senderPort(), clock_.nsecsElapsed() / 1000);
        } else if (event.type == 203) {
          ended_ = true;
          Report();
          reports_.stop();
          std::cout << "RTCP BYE received; finish queued pictures" << std::endl;
        }
      }
    }
  }

  // 接收统计需要媒体源已确认；arrival 保留原始到达时刻，使 DLSR 不少算等待时间。
  void AcceptSr(const SenderReport& sender, quint16 port, qint64 arrival) {
    peer_rtcp_port_ = port;
    stats_.OnSr(sender, arrival);
    ++sender_reports_;
    std::cout << "RTCP SR received packets=" << sender.packets
              << " ntp=" << sender.ntp << " rtp_timestamp=" << sender.timestamp << std::endl;
  }

  // RR 只包含观测结果，不调用重传、不修改编码器码率。
  void Report() {
    if (!stats_.ready() || !peer_rtcp_port_) return;
    const auto report = stats_.Report(receiver_ssrc_, clock_.nsecsElapsed() / 1000);
    const auto bytes = EncodeRr(report, "rtp-receiver@lesson");
    if (rtcp_.writeDatagram(bytes, peer_, peer_rtcp_port_) != bytes.size())
      throw std::runtime_error("RR send failed");
    ++receiver_reports_;
    std::cout << "RTCP RR sent lost=" << report.lost << " fraction=" << int(report.fraction)
              << " jitter_ticks=" << report.jitter << " lsr=" << report.lsr
              << " dlsr=" << report.dlsr << std::endl;
  }

  void Summary() const {
    std::cout << "receiver summary received=" << stats_.received() << " decoded=" << decoded_
              << " presented=" << presented_ << " gaps=" << gaps_ << " rejected=" << reorder_.rejected()
              << " damaged_units=" << assembler_.dropped() << " invalid=" << invalid_
              << " sr=" << sender_reports_ << " rr=" << receiver_reports_
              << " display_dropped=" << display_dropped_ << std::endl;
  }

  QUdpSocket rtp_, rtcp_;    // 接收媒体与收发报告的 UDP socket。
  QTimer tick_, reports_;    // 10ms 播放调度与一秒报告调度。
  QElapsedTimer clock_;      // 本进程的单调时间原点。
  VideoView* view_ = nullptr; // 复用已有教学项目的等比例画布，由窗口拥有。
  QLabel* status_ = nullptr; // 展示接收和恢复计数，由窗口拥有。
  QHostAddress peer_;        // 第一个有效发送者的地址；会话期间不接受其他发送者。
  quint16 peer_rtp_port_ = 0, peer_rtcp_port_ = 0; // 已绑定发送者的两个端口。
  quint32 source_ = 0;       // 已选发送者的 SSRC。
  quint32 receiver_ssrc_ = QRandomGenerator::global()->generate(); // 接收端自己的报告标识。
  ReceptionStats stats_;     // 到达层统计，独立于画面是否最终被显示。
  std::optional<SenderReport> early_sr_; // 最多缓存一份先到的 SR，未匹配前不认定身份。
  QHostAddress early_sr_peer_; // 先到 SR 的来源地址，须与首个 RTP 匹配。
  quint16 early_sr_port_ = 0; // 先到 SR 的返回端口。
  qint64 early_sr_time_ = 0; // 先到 SR 的原始到达时间，最多保留两秒。
  ReorderBuffer reorder_;    // 有界乱序缓冲，保持编码依赖顺序。
  H264Assembler assembler_;  // NAL/FU-A/STAP-A→完整 Annex B 访问单元。
  VideoDecoder decoder_;     // 完整访问单元→RGB 画面。
  std::deque<Picture> pictures_; // 等待按媒体时间显示的有界图像队列。
  qint64 picture_bytes_ = 0;  // 图像队列占用的 RGB 字节数。
  bool have_timestamp_ = false, ended_ = false; // 是否已有时间基准、是否收到 BYE。
  bool waiting_recovery_ = false; // 缺包后等待解码层确认已经恢复输出。
  qint64 timestamp_ = 0, first_timestamp_ = 0; // 最近展开时间戳与首帧时间戳。
  qint64 play_start_us_ = 0, last_status_us_ = 0; // 起播时刻与界面统计更新时间。
  quint64 decoded_ = 0, presented_ = 0, gaps_ = 0; // 解码/显示帧数与重排确认的缺包数。
  quint64 invalid_ = 0, display_dropped_ = 0; // 非法报文数与显示调度丢弃帧数。
  quint64 sender_reports_ = 0, receiver_reports_ = 0; // 已接收 SR、已发送 RR 数。
  QString first_frame_, screenshot_; // 可选的学习观察输出文件，未指定时不写文件。
};
}  // namespace

int main(int argc, char* argv[]) {
  QApplication app(argc, argv);
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addOptions({{"bind", "Local IPv4 address", "address", "127.0.0.1"},
                     {"port", "RTP port; RTCP=port+1", "port", "5004"},
                     {"seconds", "Exit after N seconds; 0 waits for window close", "N", "0"},
                     {"save-first-frame", "Write the first decoded RGB image as PNG", "path"},
                     {"screenshot", "Write one window screenshot after playback begins", "path"}});
  parser.process(app);
  try {
    bool port_ok = false, seconds_ok = false;
    const int port = parser.value("port").toInt(&port_ok);
    const int seconds = parser.value("seconds").toInt(&seconds_ok);
    const QHostAddress bind(parser.value("bind"));
    if (!port_ok || port < 1024 || port > 65534 || !seconds_ok || seconds < 0 || seconds > 3600 ||
        bind.protocol() != QAbstractSocket::IPv4Protocol) throw std::runtime_error("invalid receiver options");
    ReceiverWindow window(bind, quint16(port), parser.value("save-first-frame"), parser.value("screenshot"));
    window.show();
    if (seconds) QTimer::singleShot(seconds * 1000, &app, &QCoreApplication::quit);
    return app.exec();
  } catch (const std::exception& error) {
    std::cerr << "receiver failed: " << error.what() << std::endl;
    return 1;
  }
}
