#pragma once

#include <QByteArray>
#include <QVector>
#include "rtp.h"

namespace lesson {
struct SenderReport {
  quint32 ssrc = 0;       // 发送源标识，应与媒体 RTP 头一致。
  quint64 ntp = 0;        // NTP 绝对时间：高 32 位秒、低 32 位秒的小数。
  quint32 timestamp = 0;  // 与 ntp 同一瞬间的 90 kHz 媒体时钟读数。
  quint32 packets = 0;    // 实际发出的 RTP 包数，按 32 位回绕。
  quint32 octets = 0;     // RTP 载荷字节数，不计 RTP/UDP/IP 头。
};

struct ReceiverReport {
  quint32 reporter = 0;   // 生成报告的接收端 SSRC。
  quint32 source = 0;     // 此统计块针对哪个发送源。
  quint8 fraction = 0;    // 本报告区间的丢包比例乘以 256。
  qint32 lost = 0;        // 从起点累计的有符号丢包数，线上使用 24 位。
  quint32 highest = 0;    // 周期数与最大序号组成的展开最高序号。
  quint32 jitter = 0;     // 到达间隔抖动估计，单位为 RTP 时钟 tick。
  quint32 lsr = 0;        // 最近 SR 中 NTP 时间戳的中间 32 位。
  quint32 dlsr = 0;       // 收到该 SR 后经过的时间，单位为 1/65536 秒。
};

struct RtcpEvent {
  int type = 0;                     // 200=SR、201=RR、203=BYE。
  quint32 ssrc = 0;                  // 报告的发起者，或 BYE 的离开者。
  SenderReport sender;               // SR 固定发送信息；仅在 type=200 时有效。
  QVector<ReceiverReport> reports;   // SR/RR 中的接收报告块。
};

// 将当前 UTC 时间转换为 NTP 格式；只作同步/RTT 观测，不控制播放等待。
quint64 NtpNow();
// 生成 SR+SDES(CNAME)，结束时追加 BYE；同一个 UDP 报文可含多个 RTCP 包。
QByteArray EncodeSr(const SenderReport& report, const QByteArray& cname, bool bye = false);
// 生成含一个统计块的 RR，再追加本接收端的 SDES(CNAME)。
QByteArray EncodeRr(const ReceiverReport& report, const QByteArray& cname);
// 按每个 RTCP 包的 length 遍历复合报文，校验所有已访问字段的边界。
bool ParseRtcp(const QByteArray& bytes, QVector<RtcpEvent>& events);

class ReceptionStats {
 public:
  // 接收统计在到达时更新，早于重排/播放；重复和迟到包也计入 RFC 的 received。
  bool Observe(const RtpPacket& packet, qint64 now_us);
  // 保留 SR 的时钟对和单调时钟到达时刻，用于 RR 的 LSR/DLSR。
  void OnSr(const SenderReport& report, qint64 now_us);
  // 计算累计和区间统计；调用后推进 fraction lost 的区间起点。
  ReceiverReport Report(quint32 reporter, qint64 now_us);
  bool ready() const { return initialized_; }  // 尚无 RTP 时不生成含 source 的 RR。
  quint64 received() const { return received_; }  // 所有有效同源 RTP 包的接收数。
 private:
  bool initialized_ = false;  // 是否已由第一个 RTP 包建立统计起点。
  quint32 source_ = 0;        // 只统计本场已经选定的一个发送源。
  qint64 base_ = 0;           // 首包展开序号，加入前已丢的数据不在期望范围内。
  qint64 highest_ = 0;        // 迄今最大的展开序号。
  qint64 timestamp_ = 0;      // 最近一次展开的 RTP 时间戳。
  qint64 transit_ = 0;        // 上次“到达时钟-媒体时钟”的差值。
  double jitter_ = 0;         // 按 1/16 平滑后的到达间隔抖动。
  quint64 received_ = 0;      // 包括重复包，累计丢包可能因此为负。
  qint64 expected_before_ = 0; // 上份 RR 时的期望包数。
  quint64 received_before_ = 0; // 上份 RR 时的已收包数。
  quint32 lsr_ = 0;           // 最近 SR 的 NTP 中间 32 位。
  qint64 sr_arrival_us_ = 0;  // 最近 SR 到达的单调时间，单位微秒。
};
}  // namespace lesson
