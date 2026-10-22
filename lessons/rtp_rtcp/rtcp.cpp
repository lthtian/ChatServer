#include "rtcp.h"

#include <QDateTime>
#include <algorithm>
#include <cmath>
#include "wire.h"

namespace lesson {
namespace {
// RTCP length 的单位为 32 位字，且不包含头部所占的第一个字。
QByteArray Header(int type, int count, int size) {
  QByteArray bytes;
  bytes.append(char(0x80 | count));
  bytes.append(char(type));
  Put16(bytes, quint16(size / 4 - 1));
  return bytes;
}

// CNAME 标识逻辑参与者，SR/RR 自身不含可读名称；END 后按四字节补齐。
QByteArray Sdes(quint32 ssrc, const QByteArray& cname) {
  const auto name = cname.left(255);
  QByteArray payload;
  Put32(payload, ssrc);
  payload.append(char(1));
  payload.append(char(name.size()));
  payload.append(name).append(char(0));
  while (payload.size() % 4) payload.append(char(0));
  return Header(202, 1, 4 + payload.size()) + payload;
}

// 解析一个接收统计块，24 位丢包数需要显式符号扩展。
ReceiverReport ReadReport(const char* p, quint32 reporter) {
  ReceiverReport report;
  report.reporter = reporter;
  report.source = Read32(p);
  report.fraction = quint8(p[4]);
  const quint32 loss = Read32(p + 4) & 0xffffff;
  report.lost = loss & 0x800000 ? qint32(qint64(loss) - 0x1000000) : qint32(loss);
  report.highest = Read32(p + 8);
  report.jitter = Read32(p + 12);
  report.lsr = Read32(p + 16);
  report.dlsr = Read32(p + 20);
  return report;
}
}  // namespace

quint64 NtpNow() {
  const quint64 milliseconds = quint64(QDateTime::currentMSecsSinceEpoch());
  const quint64 seconds = milliseconds / 1000 + 2208988800ULL;
  return (seconds << 32) | ((milliseconds % 1000) << 32) / 1000;
}

QByteArray EncodeSr(const SenderReport& report, const QByteArray& cname, bool bye) {
  QByteArray bytes = Header(200, 0, 28);
  Put32(bytes, report.ssrc);
  Put64(bytes, report.ntp);
  Put32(bytes, report.timestamp);
  Put32(bytes, report.packets);
  Put32(bytes, report.octets);
  bytes.append(Sdes(report.ssrc, cname));
  if (bye) { bytes.append(Header(203, 1, 8)); Put32(bytes, report.ssrc); }
  return bytes;
}

QByteArray EncodeRr(const ReceiverReport& report, const QByteArray& cname) {
  QByteArray bytes = Header(201, 1, 32);
  Put32(bytes, report.reporter);
  Put32(bytes, report.source);
  Put32(bytes, (quint32(report.fraction) << 24) | (quint32(report.lost) & 0xffffff));
  Put32(bytes, report.highest);
  Put32(bytes, report.jitter);
  Put32(bytes, report.lsr);
  Put32(bytes, report.dlsr);
  bytes.append(Sdes(report.reporter, cname));
  return bytes;
}

bool ParseRtcp(const QByteArray& bytes, QVector<RtcpEvent>& events) {
  events.clear();
  int offset = 0;
  while (offset + 4 <= bytes.size()) {
    const char* p = bytes.constData() + offset;
    if ((quint8(p[0]) >> 6) != 2) return false;
    const int count = quint8(p[0]) & 31;
    const int size = (int(Read16(p + 2)) + 1) * 4;
    if (size < 4 || size > bytes.size() - offset) return false;
    int content_size = size;
    if (quint8(p[0]) & 0x20) {
      const int padding = quint8(p[size - 1]);
      if (offset + size != bytes.size() || !padding || padding > size - 4) return false;
      content_size -= padding;
    }
    const int type = quint8(p[1]);
    if (type == 200 || type == 201) {
      const int fixed = type == 200 ? 28 : 8;
      if (content_size < fixed + count * 24) return false;
      RtcpEvent event;
      event.type = type;
      event.ssrc = Read32(p + 4);
      if (type == 200) {
        event.sender = {event.ssrc, Read64(p + 8), Read32(p + 16), Read32(p + 20), Read32(p + 24)};
      }
      for (int i = 0; i < count; ++i)
        event.reports.push_back(ReadReport(p + fixed + i * 24, event.ssrc));
      events.push_back(std::move(event));
    } else if (type == 203) {
      if (content_size < 4 + count * 4) return false;
      for (int i = 0; i < count; ++i) {
        RtcpEvent event;
        event.type = type;
        event.ssrc = Read32(p + 4 + i * 4);
        events.push_back(std::move(event));
      }
    }
    // SDES/其他扩展按合法 length 跳过，本例只消费 SR、RR 和 BYE。
    offset += size;
  }
  return !bytes.isEmpty() && offset == bytes.size();
}

bool ReceptionStats::Observe(const RtpPacket& packet, qint64 now_us) {
  if (!initialized_) {
    initialized_ = true;
    source_ = packet.ssrc;
    base_ = highest_ = packet.sequence;
    timestamp_ = packet.timestamp;
    transit_ = now_us * kClockRate / 1000000 - timestamp_;
  }
  if (packet.ssrc != source_) return false;
  const qint64 sequence = Unwrap(packet.sequence, highest_, 16);
  if (std::abs(sequence - highest_) > 4096) return false;
  highest_ = std::max(highest_, sequence);
  timestamp_ = Unwrap(packet.timestamp, timestamp_, 32);
  const qint64 transit = now_us * kClockRate / 1000000 - timestamp_;
  // 用“相邻到达间隔与相邻媒体间隔的差”估计抖动，不需要两端电脑校时。
  if (received_) jitter_ += (double(std::abs(transit - transit_)) - jitter_) / 16.0;
  transit_ = transit;
  ++received_;
  return true;
}

void ReceptionStats::OnSr(const SenderReport& report, qint64 now_us) {
  if (!initialized_ || report.ssrc != source_) return;
  lsr_ = quint32(report.ntp >> 16);
  sr_arrival_us_ = now_us;
}

ReceiverReport ReceptionStats::Report(quint32 reporter, qint64 now_us) {
  const qint64 expected = highest_ - base_ + 1;
  const qint64 interval = expected - expected_before_;
  const qint64 missing = interval - qint64(received_ - received_before_);
  ReceiverReport report;
  report.reporter = reporter;
  report.source = source_;
  if (interval > 0 && missing > 0)
    report.fraction = quint8(std::min<qint64>(255, missing * 256 / interval));
  report.lost = qint32(std::clamp<qint64>(expected - qint64(received_), -8388608, 8388607));
  report.highest = quint32(highest_);
  report.jitter = quint32(jitter_);
  report.lsr = lsr_;
  if (lsr_) report.dlsr = quint32(std::max<qint64>(0, now_us - sr_arrival_us_) * 65536 / 1000000);
  expected_before_ = expected;
  received_before_ = received_;
  return report;
}
}  // namespace lesson
