#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QNetworkDatagram>
#include <QRandomGenerator>
#include <QTimer>
#include <QUdpSocket>
#include <algorithm>
#include <iostream>
#include <limits>
#include "ffmpeg_support.h"
#include "rtcp.h"

namespace {
using namespace lesson;
constexpr AVRational kUs{1, 1000000};

// 此类只从本地文件取出一条 H.264 轨道，不解码，不把其他轨道发到同一 RTP 会话。
class FileReader {
 public:
  explicit FileReader(const QString& path) {
    AVFormatContext* raw = nullptr;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    const int result = avformat_open_input(&raw, path.toUtf8().constData(), nullptr, &options);
    av_dict_free(&options);
    input_.reset(raw);
    CheckAv(result, "avformat_open_input");
    CheckAv(avformat_find_stream_info(input_.get(), nullptr), "avformat_find_stream_info");
    stream_ = av_find_best_stream(input_.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    CheckAv(stream_, "av_find_best_stream");
    const auto* stream = input_->streams[stream_];
    if (stream->codecpar->codec_id != AV_CODEC_ID_H264 || stream->codecpar->video_delay > 0)
      throw std::runtime_error("lesson requires H.264 without B frames; see README sample command");
    // MP4 中通常用长度前缀分隔 NAL；位流过滤器把它换成 Annex B，并提供 SPS/PPS。
    const auto* implementation = av_bsf_get_by_name("h264_mp4toannexb");
    if (!implementation) throw std::runtime_error("h264_mp4toannexb unavailable");
    AVBSFContext* filter = nullptr;
    CheckAv(av_bsf_alloc(implementation, &filter), "av_bsf_alloc");
    filter_.reset(filter);
    CheckAv(avcodec_parameters_copy(filter_->par_in, stream->codecpar), "avcodec_parameters_copy");
    filter_->time_base_in = stream->time_base;
    CheckAv(av_bsf_init(filter_.get()), "av_bsf_init");
    if (!input_packet_ || !filtered_) throw std::bad_alloc();
  }

  // receive 优先；输入结束后提交一次空包，把位流过滤器中剩余结果取完。
  bool Next(QByteArray& bytes, qint64& pts_us) {
    while (true) {
      av_packet_unref(filtered_.get());
      const int result = av_bsf_receive_packet(filter_.get(), filtered_.get());
      if (result == 0) {
        if (filtered_->pts == AV_NOPTS_VALUE ||
            (filtered_->dts != AV_NOPTS_VALUE && filtered_->dts != filtered_->pts))
          throw std::runtime_error("lesson requires valid PTS and no decode/display reordering");
        pts_us = av_rescale_q(filtered_->pts, filter_->time_base_out, kUs);
        bytes = QByteArray(reinterpret_cast<const char*>(filtered_->data), filtered_->size);
        return true;
      }
      if (result == AVERROR_EOF) return false;
      if (result != AVERROR(EAGAIN)) CheckAv(result, "av_bsf_receive_packet");
      if (draining_) throw std::runtime_error("bitstream filter requested input after EOF");
      int read;
      do {
        av_packet_unref(input_packet_.get());
        read = av_read_frame(input_.get(), input_packet_.get());
      } while (read >= 0 && input_packet_->stream_index != stream_);
      if (read == AVERROR_EOF) {
        CheckAv(av_bsf_send_packet(filter_.get(), nullptr), "av_bsf_send_packet EOF");
        draining_ = true;
      } else {
        CheckAv(read, "av_read_frame");
        CheckAv(av_bsf_send_packet(filter_.get(), input_packet_.get()), "av_bsf_send_packet");
      }
    }
  }
 private:
  InputPtr input_{nullptr, CloseInput};       // 本地文件及解封装状态。
  FilterPtr filter_{nullptr, FreeFilter};     // 长度前缀→Annex B 的位流过滤器。
  PacketPtr input_packet_{av_packet_alloc(), FreePacket}; // 解封装得到的输入包。
  PacketPtr filtered_{av_packet_alloc(), FreePacket};     // 已转换的输出编码包。
  int stream_ = -1;                          // 选中的视频轨道下标。
  bool draining_ = false;                    // 是否已经向过滤器提交结束信号。
};

class Sender : public QObject {
 public:
  Sender(const QString& path, QHostAddress host, quint16 port, quint16 local_port,
         int drop_every, int reorder_every)
      : reader_(path), host_(std::move(host)), port_(port),
        drop_every_(drop_every), reorder_every_(reorder_every) {
    if (!rtp_.bind(QHostAddress::AnyIPv4, local_port, QUdpSocket::DontShareAddress) ||
        !rtcp_.bind(QHostAddress::AnyIPv4, quint16(local_port + 1), QUdpSocket::DontShareAddress))
      throw std::runtime_error("cannot bind local RTP/RTCP ports");
    pacing_.setSingleShot(true);
    pacing_.setTimerType(Qt::PreciseTimer);
    // 发送定时器只决定何时发下一份访问单元；RTCP 的收发仍由同一事件循环处理。
    connect(&pacing_, &QTimer::timeout, this, [this] { Guard([this] { Pump(); }); });
    connect(&reports_, &QTimer::timeout, this, [this] { Guard([this] { Report(false); }); });
    connect(&rtcp_, &QUdpSocket::readyRead, this, [this] { ReadReports(); });
    clock_.start();
    reports_.start(1000);
    std::cout << "sender ssrc=" << ssrc_ << " initial_seq=" << sequence_
              << " initial_timestamp=" << timestamp_base_ << " destination="
              << host_.toString().toStdString() << ':' << port_ << std::endl;
    pacing_.start(0);
  }
 private:
  // Qt 槽函数不把异常抛过事件循环；统一失败退出以保留可检查的非零退出码。
  void Guard(const std::function<void()>& action) {
    try { action(); }
    catch (const std::exception& error) {
      std::cerr << "sender failed: " << error.what() << std::endl;
      pacing_.stop(); reports_.stop(); QCoreApplication::exit(1);
    }
  }

  void Pump() {
    if (pending_.isEmpty()) {
      if (!reader_.Next(pending_, pts_us_)) {
        reports_.stop();
        // 给最后的 UDP 媒体包留出接收/重排时间，再通知会话结束。
        QTimer::singleShot(150, this, [this] { Guard([this] { Report(true); }); });
        QTimer::singleShot(400, QCoreApplication::instance(), &QCoreApplication::quit);
        std::cout << "sender finished generated=" << generated_ << " sent=" << sent_
                  << " dropped=" << dropped_ << " reordered=" << reordered_ << std::endl;
        return;
      }
      if (!started_) {
        started_ = true; first_pts_us_ = pts_us_; start_us_ = clock_.nsecsElapsed() / 1000;
      }
      if (pts_us_ < previous_pts_us_) throw std::runtime_error("input PTS moved backwards");
      previous_pts_us_ = pts_us_;
    }
    const qint64 now = clock_.nsecsElapsed() / 1000;
    const qint64 due = start_us_ + pts_us_ - first_pts_us_;
    if (now < due) {
      pacing_.start(int(std::min<qint64>(1000, (due - now + 999) / 1000)));
      return;
    }
    // 本例无 B 帧，PTS 与 DTS 一致。RTP 时间戳取 PTS，绝不拿包序号代替时间戳。
    const quint32 timestamp = timestamp_base_ + quint32((pts_us_ - first_pts_us_) * kClockRate / 1000000);
    const auto packets = Packetize(pending_, timestamp, ssrc_, sequence_);
    QByteArray held;
    for (const auto& bytes : packets) {
      ++generated_;
      if (drop_every_ && generated_ % quint64(drop_every_) == 0) { ++dropped_; continue; }
      if (!held.isEmpty()) {
        Send(bytes); Send(held); held.clear(); ++reordered_;
      } else if (reorder_every_ && generated_ % quint64(reorder_every_) == 0) {
        held = bytes;
      } else Send(bytes);
    }
    if (!held.isEmpty()) Send(held);
    pending_.clear();
    pacing_.start(0);  // 把控制权还给事件循环，不用 while 把整个文件一次发完。
  }

  // 一次 writeDatagram 对应一个 UDP 数据报；计数只在实际提交成功后增加。
  void Send(const QByteArray& bytes) {
    if (rtp_.writeDatagram(bytes, host_, port_) != bytes.size())
      throw std::runtime_error("RTP send failed: " + rtp_.errorString().toStdString());
    ++sent_; octets_ += quint64(bytes.size() - 12);
    if (sent_ <= 6) {
      RtpPacket packet; ParseRtp(bytes, packet);
      std::cout << "RTP send seq=" << packet.sequence << " ts=" << packet.timestamp
                << " marker=" << packet.marker << " nal_type=" << (quint8(packet.payload[0]) & 31)
                << " bytes=" << bytes.size() << std::endl;
    }
  }

  void Report(bool bye) {
    if (!started_) return;
    const qint64 elapsed = clock_.nsecsElapsed() / 1000 - start_us_;
    SenderReport report{ssrc_, NtpNow(), timestamp_base_ + quint32(elapsed * kClockRate / 1000000),
                        quint32(sent_), quint32(octets_)};
    // SR 的 RTP timestamp 是“此刻”的媒体时钟，不是简单复制最后一包的 timestamp。
    const auto bytes = EncodeSr(report, "rtp-sender@lesson", bye);
    if (rtcp_.writeDatagram(bytes, host_, quint16(port_ + 1)) != bytes.size())
      throw std::runtime_error("RTCP send failed");
    std::cout << "RTCP SR packets=" << sent_ << " octets=" << octets_
              << " rtp_timestamp=" << report.timestamp << " bye=" << bye << std::endl;
  }

  void ReadReports() {
    while (rtcp_.hasPendingDatagrams()) {
      const auto datagram = rtcp_.receiveDatagram(2048);
      if (datagram.senderAddress() != host_ || datagram.senderPort() != port_ + 1) continue;
      QVector<RtcpEvent> events;
      if (!ParseRtcp(datagram.data(), events)) continue;
      for (const auto& event : events) for (const auto& report : event.reports) {
        if (report.source != ssrc_) continue;
        const quint32 now = quint32(NtpNow() >> 16);
        // A/LSR/DLSR 都按 16.16 秒制计算；减掉接收端等候时间才是往返估计。
        const quint32 delay = now - report.lsr - report.dlsr;
        std::cout << "RTCP RR lost=" << report.lost << " fraction=" << int(report.fraction)
                  << " highest=" << report.highest << " jitter_ticks=" << report.jitter;
        if (report.lsr && delay < 60 * 65536U)
          std::cout << " rtt_ms=" << double(delay) * 1000 / 65536;
        std::cout << std::endl;
      }
    }
  }

  FileReader reader_;         // 编码数据来源；与网络层和接收解码器分开。
  QHostAddress host_;         // 目标 IPv4 地址。
  quint16 port_;             // 目标 RTP 端口，RTCP 使用其后一个端口。
  int drop_every_, reorder_every_; // 本机实验的可选丢包、交换发送顺序间隔。
  QUdpSocket rtp_, rtcp_;     // 两个 UDP socket；RTCP 是双向控制通道。
  QTimer pacing_, reports_;   // 媒体发送调度和教学用一秒 RTCP 调度。
  QElapsedTimer clock_;       // 单调时钟，不受 Windows 系统时间修改影响。
  quint32 ssrc_ = QRandomGenerator::global()->generate(); // 每场随机发送源。
  quint32 timestamp_base_ = QRandomGenerator::global()->generate(); // 随机 RTP 时间起点。
  quint16 sequence_ = quint16(QRandomGenerator::global()->generate()); // 随机起始包号。
  QByteArray pending_;        // 只预读一个访问单元，不把整个文件装入内存。
  qint64 pts_us_ = 0;         // 预读单元在文件内的显示时刻。
  qint64 first_pts_us_ = 0;   // 本文件首个单元的 PTS。
  qint64 previous_pts_us_ = std::numeric_limits<qint64>::min(); // 检查单调性。
  qint64 start_us_ = 0;       // 首个单元对应的单调时钟起点。
  bool started_ = false;      // 是否已建立媒体与单调时钟的对应关系。
  quint64 generated_ = 0, sent_ = 0, octets_ = 0; // 分包数、实发数、实发载荷字节。
  quint64 dropped_ = 0, reordered_ = 0; // 主动丢弃包数和交换顺序的次数。
};

// 命令行中的端口和实验间隔先验证，避免隐式截断成 quint16。
int Number(const QCommandLineParser& parser, const QString& option, int low, int high) {
  bool ok = false;
  const int value = parser.value(option).toInt(&ok);
  if (!ok || value < low || value > high) throw std::runtime_error("invalid option: " + option.toStdString());
  return value;
}
}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  QCommandLineParser parser;
  parser.setApplicationDescription("H.264 RTP sender; video only, no B frames");
  parser.addHelpOption();
  parser.addPositionalArgument("file", "Local H.264 MP4/Annex B file");
  parser.addOptions({{"host", "Destination IPv4", "address", "127.0.0.1"},
                     {"port", "Destination RTP port; RTCP=port+1", "port", "5004"},
                     {"local-port", "Local RTP port; RTCP=port+1", "port", "5006"},
                     {"drop-every", "Discard every Nth generated RTP packet; 0 disables", "N", "0"},
                     {"reorder-every", "Send the next packet before every Nth packet", "N", "0"}});
  parser.process(app);
  try {
    if (parser.positionalArguments().size() != 1) throw std::runtime_error("expected one input file; use --help");
    const QHostAddress host(parser.value("host"));
    if (host.protocol() != QAbstractSocket::IPv4Protocol) throw std::runtime_error("expected numeric IPv4 address");
    Sender sender(parser.positionalArguments()[0], host,
        quint16(Number(parser, "port", 1024, 65534)), quint16(Number(parser, "local-port", 1024, 65534)),
        Number(parser, "drop-every", 0, 1000000), Number(parser, "reorder-every", 0, 1000000));
    return app.exec();
  } catch (const std::exception& error) {
    std::cerr << "sender failed: " << error.what() << std::endl;
    return 1;
  }
}
