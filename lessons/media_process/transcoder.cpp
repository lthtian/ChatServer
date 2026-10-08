#include "transcoder.h"

#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

extern "C" {
#include <libavutil/opt.h>
}

namespace media {

// 每档单独读取源文件，顺序执行，避免同时运行两套解码/编码缓存。
void TranscodeFile(const std::string& path, const std::string& destination,
               const Rendition& rendition) {
  const auto started = std::chrono::steady_clock::now();
  auto input = OpenInput(path);
  Output output(destination);
  AVStream* video = nullptr;
  AVStream* audio = nullptr;
  for (unsigned int i = 0; i < input->nb_streams; ++i) {
    AVStream* stream = input->streams[i];
    if (!video && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) video = stream;
    if (!audio && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) audio = stream;
  }
  if (!video) throw std::runtime_error("no video stream");
  TrackTranscoder video_track(input.get(), video, output, rendition);
  std::unique_ptr<TrackTranscoder> audio_track;
  if (audio) audio_track = std::make_unique<TrackTranscoder>(input.get(), audio, output, rendition);
  Check(av_dict_copy(&output.context()->metadata, input->metadata, 0), "av_dict_copy");
  output.WriteHeader();
  auto packet = CreatePacket();
  while (true) {
    const int result = av_read_frame(input.get(), packet.get());
    if (result == AVERROR_EOF) break;
    Check(result, "av_read_frame");
    if (packet->stream_index == video->index) video_track.Send(packet.get());
    else if (audio && packet->stream_index == audio->index) audio_track->Send(packet.get());
    av_packet_unref(packet.get());
  }
  // 输入 EOF 只结束了读包；各处理模块仍可能保留结果。
  video_track.Finish();
  if (audio_track) audio_track->Finish();
  output.Finish();
  const double elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - started).count();
  std::cout << "output=" << destination << " bytes=" << QFileInfo(QString::fromStdString(destination)).size()
            << " elapsed_s=" << std::fixed << std::setprecision(3) << elapsed << '\n';
}


TrackTranscoder::TrackTranscoder(AVFormatContext* input, AVStream* stream,
                               Output& output, const Rendition& rendition)
    : source_(stream), output_(output),
      decoder_(CreateCodec(avcodec_find_decoder(stream->codecpar->codec_id))),
      encoder_(CreateCodec(stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
                               ? avcodec_find_encoder_by_name("libx264")
                               : avcodec_find_encoder(AV_CODEC_ID_AAC))),
      decoded_(CreateFrame()), encoded_(CreatePacket()),
      scaler_(nullptr, [](SwsContext* p) { sws_freeContext(p); }),
      resampler_(nullptr, [](SwrContext* p) { swr_free(&p); }),
      fifo_(nullptr, &av_audio_fifo_free) {
  origin_us_ = input->start_time == AV_NOPTS_VALUE ? 0 : input->start_time;
  Check(avcodec_parameters_to_context(decoder_.get(), source_->codecpar),
        "avcodec_parameters_to_context");
  decoder_->pkt_timebase = source_->time_base;
  decoder_->thread_count = 2;
  Check(avcodec_open2(decoder_.get(), decoder_->codec, nullptr), "open decoder");
  encoder_->thread_count = 2;
  // MP4 将 SPS/PPS 等编码配置放在容器头中，而非要求每包重复携带。
  if (output_.context()->oformat->flags & AVFMT_GLOBALHEADER)
    encoder_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  if (source_->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) ConfigureVideo(input, rendition);
  else ConfigureAudio(rendition);
  Check(avcodec_open2(encoder_.get(), encoder_->codec, nullptr), "open encoder");
  target_ = output_.AddStream();
  // 转码的输出参数来自已打开的编码器，不能复制输入的 codecpar。
  Check(avcodec_parameters_from_context(target_->codecpar, encoder_.get()),
        "avcodec_parameters_from_context");
  target_->time_base = encoder_->time_base;
  target_->avg_frame_rate = encoder_->framerate;
  target_->sample_aspect_ratio = encoder_->sample_aspect_ratio;
  Check(av_dict_copy(&target_->metadata, source_->metadata, 0), "av_dict_copy");
}

TrackTranscoder::~TrackTranscoder() { av_channel_layout_uninit(&input_layout_); }

void TrackTranscoder::ConfigureVideo(AVFormatContext* input, const Rendition& rendition) {
  if (decoder_->width < 2 || decoder_->height < rendition.height)
    throw std::runtime_error("input is too small for requested rendition; upscaling is disabled");
  encoder_->height = rendition.height;
  encoder_->width = static_cast<int>(av_rescale(decoder_->width, rendition.height,
                                               decoder_->height)) / 2 * 2;
  if (encoder_->width < 2) throw std::runtime_error("invalid output width");
  encoder_->pix_fmt = AV_PIX_FMT_YUV420P;
  encoder_->time_base = AVRational{1, 90000};
  encoder_->framerate = av_guess_frame_rate(input, source_, nullptr);
  if (encoder_->framerate.num <= 0 || encoder_->framerate.den <= 0)
    throw std::runtime_error("cannot determine nominal frame rate");
  AVRational sar = decoder_->sample_aspect_ratio;
  if (sar.num <= 0 || sar.den <= 0) sar = AVRational{1, 1};
  // 偶数宽度的取整不改变显示比例：显示宽高比 = width / height * SAR。
  encoder_->sample_aspect_ratio = av_mul_q(sar, av_div_q(
      AVRational{decoder_->width, decoder_->height},
      AVRational{encoder_->width, encoder_->height}));
  encoder_->color_range = decoder_->color_range;
  encoder_->colorspace = decoder_->colorspace;
  encoder_->color_primaries = decoder_->color_primaries;
  encoder_->color_trc = decoder_->color_trc;
  encoder_->chroma_sample_location = decoder_->chroma_sample_location;
  encoder_->gop_size = std::max(1, static_cast<int>(std::lround(av_q2d(encoder_->framerate) * 2)));
  encoder_->max_b_frames = 2;
  align_segments_ = rendition.align_segments;
  if (align_segments_) {
    // VFR 也按时间切片，固定帧数 GOP 不再提前插入切点。
    encoder_->gop_size = 1000000;
    Check(av_opt_set_int(encoder_->priv_data, "forced-idr", 1, 0), "set forced IDR");
  }
  encoder_->rc_max_rate = rendition.max_video_rate;
  encoder_->rc_buffer_size = rendition.max_video_rate * 2;
  Check(av_opt_set(encoder_->priv_data, "preset", "veryfast", 0), "set preset");
  Check(av_opt_set(encoder_->priv_data, "crf", "23", 0), "set crf");
  Check(av_opt_set(encoder_->priv_data, "x264-params", "scenecut=0:open-gop=0", 0), "set GOP");
  std::cout << "video size=" << encoder_->width << 'x' << encoder_->height
            << " pixel_format=yuv420p crf=23 preset=veryfast max_rate="
            << rendition.max_video_rate << " gop_frames=" << encoder_->gop_size << '\n';
}

void TrackTranscoder::ConfigureAudio(const Rendition& rendition) {
  encoder_->sample_rate = 48000;
  encoder_->sample_fmt = AV_SAMPLE_FMT_FLTP;
  encoder_->time_base = AVRational{1, encoder_->sample_rate};
  const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
  Check(av_channel_layout_copy(&encoder_->ch_layout, &stereo), "copy output channel layout");
  encoder_->bit_rate = rendition.audio_rate;
  fifo_.reset(av_audio_fifo_alloc(encoder_->sample_fmt, encoder_->ch_layout.nb_channels, 1));
  if (!fifo_) throw std::bad_alloc();
  std::cout << "audio sample_rate=48000 layout=stereo sample_format=fltp bit_rate="
            << rendition.audio_rate << '\n';
}

void TrackTranscoder::Send(const AVPacket* packet) {
  int result = avcodec_send_packet(decoder_.get(), packet);
  if (result == AVERROR(EAGAIN)) {
    ReceiveFrames();
    result = avcodec_send_packet(decoder_.get(), packet);
  }
  Check(result, "avcodec_send_packet");
  const int received = ReceiveFrames();
  if (!packet && received != AVERROR_EOF)
    throw std::runtime_error("decoder requested input while draining");
}

int TrackTranscoder::ReceiveFrames() {
  while (true) {
    const int result = avcodec_receive_frame(decoder_.get(), decoded_.get());
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return result;
    Check(result, "avcodec_receive_frame");
    if (source_->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) ConvertVideo(decoded_.get());
    else ConvertAudio(decoded_.get());
    ++frame_count_;
    av_frame_unref(decoded_.get());
  }
}

void TrackTranscoder::ConvertVideo(const AVFrame* source) {
  if (source->flags & AV_FRAME_FLAG_INTERLACED)
    throw std::runtime_error("interlaced input requires deinterlacing, outside this lesson");
  if (source->color_trc == AVCOL_TRC_SMPTE2084 || source->color_trc == AVCOL_TRC_ARIB_STD_B67)
    throw std::runtime_error("HDR input requires a separate color pipeline");
  if (source->width != source_->codecpar->width || source->height != source_->codecpar->height)
    throw std::runtime_error("video dimensions changed");
  auto scaled = CreateFrame();
  scaled->format = encoder_->pix_fmt;
  scaled->width = encoder_->width;
  scaled->height = encoder_->height;
  scaled->sample_aspect_ratio = encoder_->sample_aspect_ratio;
  scaled->color_range = encoder_->color_range;
  scaled->colorspace = encoder_->colorspace;
  scaled->color_primaries = encoder_->color_primaries;
  scaled->color_trc = encoder_->color_trc;
  scaled->chroma_location = encoder_->chroma_sample_location;
  // 按实际像素格式建立缩放上下文；参数相同时复用，输出仍是原始图像。
  scaler_.reset(sws_getCachedContext(scaler_.release(), source->width, source->height,
      static_cast<AVPixelFormat>(source->format), scaled->width, scaled->height,
      static_cast<AVPixelFormat>(scaled->format), SWS_BILINEAR, nullptr, nullptr, nullptr));
  if (!scaler_) throw std::bad_alloc();
  // sws_scale_frame 按目标尺寸/格式分配像素缓冲；输出仍是原始图像，不是 H.264。
  Check(sws_scale_frame(scaler_.get(), scaled.get(), source), "sws_scale_frame");
  scaled->pts = source->best_effort_timestamp == AV_NOPTS_VALUE ? next_video_pts_
      : av_rescale_q(source->best_effort_timestamp, source_->time_base, encoder_->time_base)
          - av_rescale_q(origin_us_, AV_TIME_BASE_Q, encoder_->time_base);
  scaled->duration = std::max<int64_t>(1, source->duration > 0
      ? av_rescale_q(source->duration, source_->time_base, encoder_->time_base)
      : av_rescale_q(1, av_inv_q(encoder_->framerate), encoder_->time_base));
  if (last_video_pts_ != AV_NOPTS_VALUE && scaled->pts <= last_video_pts_)
    throw std::runtime_error("video timestamps are not increasing");
  last_video_pts_ = scaled->pts;
  next_video_pts_ = scaled->pts + scaled->duration;
  if (align_segments_) {
    if (next_key_pts_ == AV_NOPTS_VALUE) next_key_pts_ = scaled->pts;
    if (scaled->pts >= next_key_pts_) {
      scaled->pict_type = AV_PICTURE_TYPE_I;
      do { next_key_pts_ += 180000; } while (next_key_pts_ <= scaled->pts);
    }
  }
  // 解码器返回显示顺序的帧；编码器会自行生成含 B 帧重排的 PTS/DTS。
  Encode(scaled.get());
}

void TrackTranscoder::ConvertAudio(const AVFrame* source) {
  const auto format = static_cast<AVSampleFormat>(source->format);
  if (source->sample_rate <= 0 || source->nb_samples <= 0 ||
      !av_channel_layout_check(&source->ch_layout) || av_get_bytes_per_sample(format) <= 0)
    throw std::runtime_error("invalid decoded audio parameters");
  if (!resampler_) {
    input_rate_ = source->sample_rate;
    input_format_ = format;
    Check(av_channel_layout_copy(&input_layout_, &source->ch_layout), "copy input channel layout");
    SwrContext* raw = nullptr;
    const int result = swr_alloc_set_opts2(&raw, &encoder_->ch_layout, encoder_->sample_fmt,
        encoder_->sample_rate, &input_layout_, input_format_, input_rate_, 0, nullptr);
    resampler_.reset(raw);
    Check(result, "swr_alloc_set_opts2");
    Check(swr_init(resampler_.get()), "swr_init");
    next_audio_pts_ = source->best_effort_timestamp == AV_NOPTS_VALUE ? 0
        : av_rescale_q(source->best_effort_timestamp, source_->time_base, encoder_->time_base)
            - av_rescale_q(origin_us_, AV_TIME_BASE_Q, encoder_->time_base);
  } else if (input_rate_ != source->sample_rate || input_format_ != format ||
             av_channel_layout_compare(&input_layout_, &source->ch_layout) != 0) {
    throw std::runtime_error("audio parameters changed within the stream");
  }
  // FIFO 不储存每块数据的 PTS，因此明确要求连续音轨，不能把时间空洞悄悄拼掉。
  if (source->best_effort_timestamp != AV_NOPTS_VALUE) {
    const int64_t position = av_rescale_q(source->best_effort_timestamp, source_->time_base,
                                         AVRational{1, input_rate_});
    const int64_t tolerance = std::max<int64_t>(2,
        static_cast<int64_t>(std::ceil(av_q2d(source_->time_base) * input_rate_)) + 1);
    if (next_input_sample_ != AV_NOPTS_VALUE && std::llabs(position - next_input_sample_) > tolerance)
      throw std::runtime_error("audio timestamp gap/overlap requires an explicit repair policy");
    next_input_sample_ = position + source->nb_samples;
  } else if (next_input_sample_ != AV_NOPTS_VALUE) {
    next_input_sample_ += source->nb_samples;
  }
  Resample(source);
}

int TrackTranscoder::Resample(const AVFrame* source) {
  const int bound = swr_get_out_samples(resampler_.get(), source ? source->nb_samples : 0);
  Check(bound, "swr_get_out_samples");
  auto converted = CreateFrame();
  converted->format = encoder_->sample_fmt;
  converted->sample_rate = encoder_->sample_rate;
  converted->nb_samples = std::max(1, bound);
  Check(av_channel_layout_copy(&converted->ch_layout, &encoder_->ch_layout), "copy channel layout");
  Check(av_frame_get_buffer(converted.get(), 0), "av_frame_get_buffer");
  // 指针数组借用输入平面，只增加只读限定，不复制 PCM，也不修改解码帧。
  std::vector<const uint8_t*> input_planes;
  if (source) {
    const int planes = av_sample_fmt_is_planar(input_format_) ? source->ch_layout.nb_channels : 1;
    input_planes.assign(source->extended_data, source->extended_data + planes);
  }
  const int count = swr_convert(resampler_.get(), converted->extended_data, converted->nb_samples,
                                source ? input_planes.data() : nullptr,
                                source ? source->nb_samples : 0);
  Check(count, "swr_convert");
  if (count > 0) {
    Check(av_audio_fifo_write(fifo_.get(), reinterpret_cast<void**>(converted->extended_data), count),
          "av_audio_fifo_write");
    EncodeAudio(false);
  }
  return count;
}

void TrackTranscoder::EncodeAudio(bool final) {
  const int size = encoder_->frame_size;
  if (size <= 0) throw std::runtime_error("expected a fixed-size AAC audio encoder");
  while (av_audio_fifo_size(fifo_.get()) >= size || (final && av_audio_fifo_size(fifo_.get()) > 0)) {
    auto frame = CreateFrame();
    frame->format = encoder_->sample_fmt;
    frame->sample_rate = encoder_->sample_rate;
    frame->nb_samples = std::min(size, av_audio_fifo_size(fifo_.get()));
    if (frame->nb_samples < size && !(encoder_->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME))
      throw std::runtime_error("encoder does not support a short final audio frame");
    Check(av_channel_layout_copy(&frame->ch_layout, &encoder_->ch_layout), "copy channel layout");
    Check(av_frame_get_buffer(frame.get(), 0), "av_frame_get_buffer");
    const int read = av_audio_fifo_read(fifo_.get(), reinterpret_cast<void**>(frame->extended_data),
                                      frame->nb_samples);
    if (read != frame->nb_samples) throw std::runtime_error("incomplete audio FIFO read");
    frame->pts = next_audio_pts_;
    frame->duration = frame->nb_samples;
    next_audio_pts_ += frame->nb_samples;
    sample_count_ += frame->nb_samples;
    Encode(frame.get());
  }
}

void TrackTranscoder::Encode(AVFrame* frame) {
  int result = avcodec_send_frame(encoder_.get(), frame);
  if (result == AVERROR(EAGAIN)) {
    ReceivePackets();
    result = avcodec_send_frame(encoder_.get(), frame);  // 仍提交同一帧，不能跳过。
  }
  Check(result, "avcodec_send_frame");
  const int received = ReceivePackets();
  if (!frame && received != AVERROR_EOF)
    throw std::runtime_error("encoder requested input while draining");
}

int TrackTranscoder::ReceivePackets() {
  while (true) {
    const int result = avcodec_receive_packet(encoder_.get(), encoded_.get());
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) return result;
    Check(result, "avcodec_receive_packet");
    output_.WritePacket(encoded_.get(), encoder_->time_base, target_);
    ++packet_count_;
  }
}

void TrackTranscoder::Finish() {
  Send(nullptr);
  if (frame_count_ == 0) throw std::runtime_error("selected stream contains no frames");
  if (resampler_) {
    while (Resample(nullptr) > 0) {}
    EncodeAudio(true);
  }
  Encode(nullptr);
  std::cout << "track=" << source_->index << " decoded_frames=" << frame_count_
            << " encoded_packets=" << packet_count_ << " audio_samples=" << sample_count_ << '\n';
}

void TranscodeRenditions(const std::string& input_path, const std::string& directory) {
  if (!QFileInfo(QString::fromStdString(input_path)).isFile())
    throw std::runtime_error("input file does not exist");
  const QDir folder(QString::fromStdString(directory));
  if (!folder.mkpath(".")) throw std::runtime_error("cannot create output directory");
  const Rendition renditions[] = {{480, 1200000, 128000}, {360, 700000, 96000}};
  for (const auto& rendition : renditions) {
    const auto path = folder.filePath(QString::number(rendition.height) + "p.mp4");
    if (QFileInfo::exists(path) || QFileInfo::exists(path + ".part"))
      throw std::runtime_error("output or .part file already exists: " + path.toStdString());
  }
  for (const auto& rendition : renditions) {
    const auto path = folder.filePath(QString::number(rendition.height) + "p.mp4");
    TranscodeFile(input_path, path.toStdString(), rendition);
  }
}

}  // namespace media
