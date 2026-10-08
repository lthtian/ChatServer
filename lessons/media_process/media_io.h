#pragma once

#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace media {

// 智能指针分别负责输入、编解码实例、帧和包的释放；get() 只借用对象。
using InputPtr = std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)>;
using CodecPtr = std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)>;
using FramePtr = std::unique_ptr<AVFrame, void (*)(AVFrame*)>;
using PacketPtr = std::unique_ptr<AVPacket, void (*)(AVPacket*)>;

void Check(int result, const char* operation);  // 将 FFmpeg 负错误码变成异常。
InputPtr OpenInput(const std::string& path, const char* format = nullptr);  // 打开本地文件并探测轨道。
CodecPtr CreateCodec(const AVCodec* codec);  // 分配工作实例，尚未 open。
FramePtr CreateFrame();  // 分配帧对象，尚无像素/采样缓冲。
PacketPtr CreatePacket();  // 分配包对象，尚无编码数据。

// 管理输出容器、磁盘 I/O 和半成品；轨道必须在 WriteHeader 前配置完成。
class Output {
 public:
  explicit Output(const std::string& path);  // 支持 .mp4/.mkv，拒绝覆盖已有文件。
  ~Output();  // 关闭 I/O；未完成的本次输出按尽力方式清理。
  Output(const Output&) = delete;
  Output& operator=(const Output&) = delete;
  AVFormatContext* context() const { return context_.get(); }  // 借用封装上下文。
  AVStream* AddStream();  // 添加一条输出轨道，归输出上下文所有。
  void WriteHeader();  // 打开文件并写头；封装器可能调整各轨道 time_base。
  void WritePacket(AVPacket* packet, AVRational source_time_base, AVStream* stream);
  // WritePacket 换算时间戳、设置输出轨道，封装器接管包的数据引用。
  void Finish();  // 写尾、关闭文件，再将 .part 发布成最终文件。

 private:
  std::string path_;  // 最终输出文件的 UTF-8 路径。
  std::string temporary_path_;  // 同目录 .part 文件，异常时可安全识别半成品。
  std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> context_;  // 输出上下文。
  bool opened_ = false;  // 本实例是否创建了半成品；避免清理已有文件。
};

// 复制音视频编码包，不创建解码器和编码器；忽略字幕、附件等其他轨道。
void Remux(const std::string& input_path, const std::string& output_path);

// 串行产生两个 MP4：480p 和 360p；有音轨时转为 AAC 48 kHz 双声道。
void TranscodeRenditions(const std::string& input_path, const std::string& directory);

}  // namespace media
