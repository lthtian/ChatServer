#include "media_io.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace media {

void Remux(const std::string& input_path, const std::string& output_path) {
  auto input = OpenInput(input_path);
  Output output(output_path);
  // 输入轨道号不一定等于输出轨道号；跳过字幕后尤其不能直接照搬编号。
  std::vector<AVStream*> mapping(input->nb_streams, nullptr);
  for (unsigned int i = 0; i < input->nb_streams; ++i) {
    const AVStream* source = input->streams[i];
    const auto type = source->codecpar->codec_type;
    if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) continue;
    if (source->disposition & AV_DISPOSITION_ATTACHED_PIC) continue;
    // 为输入流创建输出流
    AVStream* target = output.AddStream();
    Check(avcodec_parameters_copy(target->codecpar, source->codecpar),
          "avcodec_parameters_copy");
    target->codecpar->codec_tag = 0;  // 让目标容器选择自己的编码标识。
    target->time_base = source->time_base;
    target->avg_frame_rate = source->avg_frame_rate;
    target->sample_aspect_ratio = source->sample_aspect_ratio;
    target->disposition = source->disposition;
    Check(av_dict_copy(&target->metadata, source->metadata, 0), "av_dict_copy");
    mapping[i] = target;
  }
  if (output.context()->nb_streams == 0) throw std::runtime_error("no audio/video streams");
  Check(av_dict_copy(&output.context()->metadata, input->metadata, 0), "av_dict_copy");
  output.WriteHeader();
  auto packet = CreatePacket();
  int64_t packets = 0;
  while (true) {
    const int result = av_read_frame(input.get(), packet.get());
    if (result == AVERROR_EOF) break;
    Check(result, "av_read_frame");
    const int index = packet->stream_index;
    if (mapping[index]) {
      // 原包已经是解码顺序；不按 PTS 排序，也不逐轨道减去不同的起点。
      output.WritePacket(packet.get(), input->streams[index]->time_base, mapping[index]);
      ++packets;
    }
    av_packet_unref(packet.get());
  }
  output.Finish();
  std::cout << "remux packets=" << packets << " output=" << output_path << '\n';
}

}  // namespace media
