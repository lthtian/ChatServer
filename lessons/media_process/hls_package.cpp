#include "hls_package.h"
#include "transcoder.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace media {
namespace {
QByteArray Hash(const QString& path) {
  QFile file(path);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file))
    throw std::runtime_error("cannot hash file");
  return hash.result().toHex();
}

void Write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(bytes) != bytes.size() ||
      !file.flush()) throw std::runtime_error("cannot write output");
}

// 输入是已经转码的 MP4；HLS muxer 负责把编码包重封装成分片并生成列表。
QJsonObject Segment(const QString& path, const QDir& folder, bool fmp4) {
  auto input = OpenInput(path.toStdString());
  const auto playlist = folder.filePath("index.m3u8").toUtf8();
  AVFormatContext* raw = nullptr;
  Check(avformat_alloc_output_context2(&raw, nullptr, "hls", playlist.constData()), "allocate HLS");
  std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> output(raw, [](AVFormatContext* p) {
    if (p->pb) avio_closep(&p->pb);
    avformat_free_context(p);
  });
  if (!output) throw std::bad_alloc();
  QJsonObject result;
  QString codecs;
  for (unsigned int i = 0; i < input->nb_streams; ++i) {
    const auto* source = input->streams[i];
    auto* target = avformat_new_stream(output.get(), nullptr);
    if (!target) throw std::bad_alloc();
    Check(avcodec_parameters_copy(target->codecpar, source->codecpar), "copy HLS codec parameters");
    target->codecpar->codec_tag = 0;
    target->time_base = source->time_base;
    if (source->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      result["width"] = source->codecpar->width;
      result["height"] = source->codecpar->height;
      const auto* extra = source->codecpar->extradata;
      if (source->codecpar->extradata_size < 4 || extra[0] != 1)
        throw std::runtime_error("expected AVC configuration record");
      codecs = "avc1." + QByteArray(reinterpret_cast<const char*>(extra + 1), 3).toHex();
    }
  }
  for (unsigned int i = 0; i < input->nb_streams; ++i)
    if (input->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) codecs += ",mp4a.40.2";
  result["codecs"] = codecs;
  AVDictionary* options = nullptr;
  av_dict_set(&options, "hls_time", "2", 0);
  av_dict_set(&options, "hls_list_size", "0", 0);
  av_dict_set(&options, "hls_playlist_type", "vod", 0);
  av_dict_set(&options, "hls_flags", "independent_segments", 0);
  av_dict_set(&options, "hls_segment_type", fmp4 ? "fmp4" : "mpegts", 0);
  const auto segments = folder.filePath(fmp4 ? "seg_%05d.m4s" : "seg_%05d.ts").toUtf8();
  av_dict_set(&options, "hls_segment_filename", segments.constData(), 0);
  if (fmp4) av_dict_set(&options, "hls_fmp4_init_filename", "init.mp4", 0);
  const int header = avformat_write_header(output.get(), &options);
  av_dict_free(&options);
  Check(header, "write HLS header");
  auto packet = CreatePacket();
  while (true) {
    const int read = av_read_frame(input.get(), packet.get());
    if (read == AVERROR_EOF) break;
    Check(read, "read rendition packet");
    av_packet_rescale_ts(packet.get(), input->streams[packet->stream_index]->time_base,
                        output->streams[packet->stream_index]->time_base);
    packet->pos = -1;
    Check(av_interleaved_write_frame(output.get(), packet.get()), "write HLS packet");
    av_packet_unref(packet.get());
  }
  Check(av_write_trailer(output.get()), "finish HLS");
  // BANDWIDTH 包括音频和容器开销；从实际分片码率取峰值，留出 10% 余量。
  QFile list(QString::fromUtf8(playlist));
  if (!list.open(QIODevice::ReadOnly)) throw std::runtime_error("cannot read playlist");
  double seconds = 0, total_seconds = 0, peak = 0;
  for (const auto& line : list.readAll().split('\n')) {
    if (line.startsWith("#EXTINF:")) seconds = line.mid(8).split(',')[0].toDouble();
    else if (!line.trimmed().isEmpty() && !line.startsWith('#')) {
      if (seconds <= 0) throw std::runtime_error("invalid segment duration");
      peak = std::max(peak, QFileInfo(folder.filePath(QString::fromUtf8(line.trimmed()))).size() * 8.0 / seconds);
      total_seconds += seconds;
      seconds = 0;
    }
  }
  result["bandwidth"] = static_cast<int>(std::ceil(peak * 1.1));
  result["duration_s"] = total_seconds;
  return result;
}
}  // namespace

void PackageHls(const std::string& path, const std::string& directory, bool fmp4) {
  const QString source = QString::fromStdString(path);
  const QFileInfo file(source);
  if (!file.isFile() || file.size() <= 0 || file.size() > 100 * 1024 * 1024)
    throw std::runtime_error("input must be a local file of at most 100 MiB");
  auto input = OpenInput(path, "mov");
  const int index = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  Check(index, "find video");
  const auto* video = input->streams[index]->codecpar;
  if (input->duration <= 0 || input->duration > 3600LL * AV_TIME_BASE)
    throw std::runtime_error("HLS lesson requires a known duration of at most one hour");
  const QString destination = QDir::cleanPath(QString::fromStdString(directory));
  const QString staging = destination + ".part";
  if (QFileInfo::exists(destination) || QFileInfo::exists(staging))
    throw std::runtime_error("output or .part directory already exists");
  if (!QDir().mkpath(staging)) throw std::runtime_error("cannot create HLS directory");
  const QDir folder(staging);
  QJsonArray variants;
  QByteArray master("#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-INDEPENDENT-SEGMENTS\n");
  std::vector<Rendition> candidates{{720, 2800000, 128000, true},
                                   {480, 1200000, 128000, true}, {360, 700000, 96000, true}};
  if (video->height < 360 && video->height >= 2)
    candidates = {{video->height / 2 * 2, 500000, 96000, true}};
  for (const auto& rendition : candidates) {
    if (rendition.height > video->height) continue;
    const QString id = QString::number(rendition.height) + "p";
    if (!folder.mkdir(id)) throw std::runtime_error("cannot create rendition directory");
    const QString temporary = folder.filePath(id + ".mp4");
    TranscodeFile(path, temporary.toStdString(), rendition);
    auto variant = Segment(temporary, QDir(folder.filePath(id)), fmp4);
    if (!QFile::remove(temporary)) throw std::runtime_error("cannot remove intermediate MP4");
    variant["id"] = id;
    variant["playlist"] = id + "/index.m3u8";
    master += "#EXT-X-STREAM-INF:BANDWIDTH=" + QByteArray::number(variant["bandwidth"].toInt()) +
        ",RESOLUTION=" + QByteArray::number(variant["width"].toInt()) + "x" +
        QByteArray::number(variant["height"].toInt()) + ",CODECS=\"" + variant["codecs"].toString().toUtf8() + "\"\n" +
        variant["playlist"].toString().toUtf8() + "\n";
    variants.append(variant);
  }
  if (variants.isEmpty()) throw std::runtime_error("no eligible video rendition");
  Write(folder.filePath("master.m3u8"), master);
  QJsonArray assets;
  qint64 total = 0;
  QDirIterator files(staging, QDir::Files, QDirIterator::Subdirectories);
  while (files.hasNext()) {
    const QString asset = files.next();
    const auto info = files.fileInfo();
    total += info.size();
    if (total > 256 * 1024 * 1024 || assets.size() >= 4096)
      throw std::runtime_error("HLS output exceeds lesson storage limits");
    const QString extension = info.suffix();
    const QString mime = extension == "m3u8" ? "application/vnd.apple.mpegurl" :
                         extension == "ts" ? "video/mp2t" : "video/mp4";
    assets.append(QJsonObject{{"path", folder.relativeFilePath(asset)}, {"bytes", info.size()},
                             {"sha256", QString::fromLatin1(Hash(asset))}, {"mime", mime}});
  }
  const QJsonObject catalog{{"version", 1}, {"revision", QUuid::createUuid().toString(QUuid::Id128)},
      {"source_sha256", QString::fromLatin1(Hash(source))}, {"source_width", video->width},
      {"source_height", video->height}, {"segment_type", fmp4 ? "fmp4" : "ts"},
      {"variants", variants}, {"assets", assets}};
  Write(folder.filePath("catalog.json"), QJsonDocument(catalog).toJson());
  if (!QDir().rename(staging, destination)) throw std::runtime_error("cannot publish completed directory");
  std::cout << "hls=" << directory << " variants=" << variants.size() << " bytes=" << total << '\n';
}
}  // namespace media
