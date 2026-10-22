#include <QCoreApplication>
#include <iostream>
#include <stdexcept>
#include "rtcp.h"
#include "wire.h"

namespace {
using namespace lesson;

// 固定报文字节作为判据，避免只测试“自己的序列化能被自己的解析读回”。
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void HeaderChecks() {
  RtpPacket packet;
  const auto wire = QByteArray::fromHex("80e0ffff123456789abcdef06511");
  Require(ParseRtp(wire, packet), "parse fixed RTP vector");
  Require(packet.sequence == 65535 && packet.timestamp == 0x12345678 &&
          packet.ssrc == 0x9abcdef0 && packet.marker && packet.payload_type == 96 &&
          packet.payload == QByteArray::fromHex("6511"), "RTP field values");
  Require(EncodeRtp(packet) == wire, "serialize fixed RTP vector");
  // X=1，扩展长度为一个 32 位字；P=1，末尾两个字节属于 padding。
  const auto extended = QByteArray::fromHex("b0e00001123456789abcdef0bede00011122334465110002");
  Require(ParseRtp(extended, packet) && packet.payload == QByteArray::fromHex("6511"), "extension and padding");
  Require(!ParseRtp(extended.left(17), packet) && !ParseRtp(wire.left(11), packet), "truncated RTP rejected");
  Require(Unwrap(0, 65535, 16) == 65536 && Unwrap(0, 0xffffffffLL, 32) == 0x100000000LL,
          "sequence/timestamp wrap");
}

void H264Checks() {
  // NAL 头和 SPS/PPS 在这里仅用作分包重组字节测试，不冒充可解码的视频。
  const auto prefix = QByteArray::fromHex("0000000167112200000001683344");
  const auto frame = prefix + QByteArray::fromHex("0000000165") + QByteArray(4000, char(0x55));
  quint16 sequence = 65533;
  const auto packets = Packetize(frame, 90000, 42, sequence);
  Require(packets.size() > 3, "large NAL uses FU-A");
  H264Assembler assembler;
  QByteArray recovered;
  for (int i = 0; i < packets.size(); ++i) {
    RtpPacket packet;
    Require(packets[i].size() <= 1200 && ParseRtp(packets[i], packet), "MTU/header bounds");
    Require(packet.marker == (i + 1 == packets.size()), "Marker only on final packet");
    assembler.Push(packet, [&](QByteArray bytes, quint32 timestamp) {
      Require(timestamp == 90000, "same access unit timestamp"); recovered = std::move(bytes);
    });
  }
  Require(recovered == frame, "FU-A reassembly exact bytes");
  H264Assembler damaged;
  int delivered = 0;
  for (int i = 0; i < packets.size(); ++i) {
    if (i == 3) { damaged.Lost(); continue; }
    RtpPacket packet; ParseRtp(packets[i], packet);
    damaged.Push(packet, [&](QByteArray, quint32) { ++delivered; });
  }
  Require(delivered == 0, "missing FU fragment never decoded");
  for (const auto& bytes : packets) {
    RtpPacket packet; ParseRtp(bytes, packet); packet.timestamp += 90000;
    damaged.Push(packet, [&](QByteArray, quint32) { ++delivered; });
  }
  Require(delivered == 1, "next IDR recovers");
  // 一个实际 STAP-A 布局：两个字节长度后跟完整 NAL。
  H264Assembler aggregated;
  RtpPacket stap;
  stap.marker = true;
  stap.payload = QByteArray::fromHex("780003671122000368334400026555");
  aggregated.Push(stap, [&](QByteArray bytes, quint32) {
    Require(bytes == prefix + QByteArray::fromHex("000000016555"), "STAP-A NAL lengths"); ++delivered;
  });
  Require(delivered == 2, "STAP-A delivered");
}

void BufferChecks() {
  ReorderBuffer buffer;
  QVector<quint16> order;
  qint64 missing = 0;
  const auto deliver = [&](const RtpPacket& packet) { order.push_back(packet.sequence); };
  const auto gap = [&](qint64 count) { missing += count; };
  const auto insert = [&](quint16 sequence, qint64 time) {
    RtpPacket packet; packet.sequence = sequence; buffer.Insert(packet, time); buffer.Drain(time, deliver, gap);
  };
  insert(65534, 0); insert(0, 1000); insert(65535, 2000); insert(2, 3000);
  Require(order == QVector<quint16>({65534, 65535, 0}), "reorder across wrap");
  buffer.Drain(53000, deliver, gap);
  Require(missing == 1 && order.back() == 2, "missing sequence expires after 50ms");
  insert(1, 54000);
  Require(buffer.rejected() == 1, "late packet not redelivered");
  for (int i = 10; i < 1000; ++i) { RtpPacket p; p.sequence = quint16(i); buffer.Insert(p, 60000); }
  Require(buffer.size() <= 256, "reorder memory bounded");
}

void ReportChecks() {
  const SenderReport report{0x11223344, 0x0102030405060708ULL, 90000, 3, 999};
  const auto wire = EncodeSr(report, "sender");
  Require(wire.left(28) == QByteArray::fromHex("80c8000611223344010203040506070800015f9000000003000003e7"),
          "SR wire fields and length");
  QVector<RtcpEvent> events;
  Require(ParseRtcp(wire, events) && events.size() == 1 && events[0].sender.octets == 999, "compound SR/SDES");
  Require(!ParseRtcp(wire.left(27), events), "truncated SR rejected");
  ReceptionStats stats;
  RtpPacket packet; packet.ssrc = 42; packet.sequence = 65534; packet.timestamp = 0xfffffff0;
  Require(stats.Observe(packet, 0), "stats first packet");
  packet.sequence = 0; packet.timestamp += 3600; stats.Observe(packet, 40000);
  stats.OnSr({42, 0x123456789abcdef0ULL, 3600, 3, 1000}, 100000);
  auto rr = stats.Report(77, 600000);
  Require(rr.lost == 1 && rr.fraction == 85 && rr.highest == 65536 && rr.jitter == 0,
          "expected/received, 24-bit loss and sequence cycles");
  Require(rr.lsr == 0x56789abc && rr.dlsr == 32768, "LSR and DLSR units");
  rr.lost = -2;
  Require(ParseRtcp(EncodeRr(rr, "receiver"), events) && events[0].reports[0].lost == -2,
          "signed 24-bit cumulative loss");
}
}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication app(argc, argv);
  try {
    HeaderChecks(); H264Checks(); BufferChecks(); ReportChecks();
    std::cout << "protocol checks passed: fixed wire vectors, FU-A/STAP-A, gaps/wrap, SR/RR" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "protocol check failed: " << error.what() << std::endl;
    return 1;
  }
}
