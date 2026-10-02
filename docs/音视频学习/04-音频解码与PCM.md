# 第四阶段：音频采样、PCM、解码与重采样

## 1. 从音轨到声音数据

```text
MP4 容器中的音频轨道
    → AAC 编码包 AVPacket
    → 音频解码器 AVCodecContext
    → 音频帧 AVFrame：一段 PCM 采样，例如 FLTP
    → SwrContext：采样率、样本格式、声道布局转换
    → S16 交错 PCM
    → 保存文件，或交给音频设备播放
```

| 概念 | 描述什么 | 例子 |
|---|---|---|
| 容器格式 | 怎样组织轨道、时间戳和编码数据 | MP4、WAV |
| 音频编码格式 | 声音怎样压缩和恢复 | AAC、MP3、Opus、FLAC |
| PCM | 以一串数值表示各时刻的声音采样 | 解码后的整数或浮点采样 |
| 样本格式 | 一个采样怎样存储，以及各声道怎样排列 | S16、FLT、FLTP |
| 采样率 | 每个声道每秒有多少个采样 | 44100 Hz、48000 Hz |
| 声道布局 | 有多少个声道，以及各声道的含义和顺序 | mono、stereo、5.1 |

AAC 与 FLTP 属于不同层次：AAC 是编码格式，FLTP 是解码后 PCM 的内存格式。具体输出由解码器和输入共同决定，不能仅凭 codec_id 假定一定输出 S16 或 FLTP。

本例同步地将整条音轨解码、转换并写入文件，不按现实时间等待，也不向声音设备提交数据。保存五秒音频可以少于五秒；真正播放还需要设备输出和缓冲管理。

## 2. 采样、频率与数值精度

### 一个采样是什么

声音可以表示为随时间变化的波形。数字音频按固定时间间隔记录波形的数值，每个记录就是一个采样。一个声道的一小段数据可以是：

```text
时刻：t0    t1    t2    t3    t4
数值：0    0.1   0.2   0.1   0
```

负数表示波形在参考零点的另一侧，不是负音量。波形变化的频率关系到音高；幅度关系到信号强弱，但实际听到的响度还受设备音量和听觉等因素影响。

### 采样率不等于声音频率

44100 Hz 表示每个声道每秒记录 44100 个数值，相邻采样间隔为 `1 / 44100` 秒。440 Hz 的正弦音表示波形每秒重复 440 次，两者不能混为一谈。

采样前需要限制信号频带；理想采样理论要求采样率高于最高信号频率的两倍，实际系统还需要给滤波器留出过渡空间。提高已有音频的采样率不会凭空恢复原本没有记录的高频细节。

### 样本格式与位深

| FFmpeg 格式 | 单个采样的存储 | 数值含义 |
|---|---|---|
| `AV_SAMPLE_FMT_U8` | 无符号 8 位，1 字节 | 0～255，静音中心为 128 |
| `AV_SAMPLE_FMT_S16` | 有符号 16 位，2 字节 | -32768～32767，静音为 0 |
| `AV_SAMPLE_FMT_S32` | 有符号 32 位，4 字节 | 更宽的整数表示，静音为 0 |
| `AV_SAMPLE_FMT_FLT` | 32 位 float，4 字节 | 满幅参考范围为 -1.0～1.0，静音为 0 |
| `AV_SAMPLE_FMT_FLTP` | 32 位 float，4 字节，平面排列 | 与 FLT 的数值类型相同，仅声道存放方式不同 |

整数位深越高，可表示的幅度等级越多。浮点数采用指数和有效位表示，不能把“32 位 float”理解成“32 位整数具有相同的均匀精度”。浮点运算中也可能出现超过满幅参考范围的值；转为整数时需要处理范围，越界削波会造成失真。

修改 `frame->format` 只是改描述，不会把 float 字节变成 int16_t。真正的格式转换需要读取原值、做数值换算并写入目标格式。

FFmpeg 的 S16 等样本格式使用机器本机字节序。本例运行于小端平台，导出的是 s16le：例如整数 `0x1234` 在文件中存为 `34 12`。位深、字节序和声道排列是三个不同属性。

## 3. 声道与平面、交错排列

双声道是两条同时进行的采样序列。立体声 stereo 指定左、右声道的含义；只知道“2 个声道”并不总能确定它们的空间位置。

假设同一段音频的左声道为 L0、L1、L2，右声道为 R0、R1、R2：

```text
交错 packed/interleaved，例如 S16、FLT：
extended_data[0] → L0 R0 L1 R1 L2 R2

平面 planar，例如 S16P、FLTP：
extended_data[0] → L0 L1 L2
extended_data[1] → R0 R1 R2
```

末尾 P 表示平面排列，不表示压缩。上述两种排列可以表达同样的声音。单声道只有一条采样序列，平面与交错的排列差异不明显，但格式枚举仍应正确设置。

平面排列在逻辑上可以理解为“声道 × 采样”的二维结构，但实际是指针表指向各声道缓冲区，不保证是连续的 C++ 二维数组。每个指针下面都保存该声道的一串采样。

三个声道的交错排列为 `A0 B0 C0 | A1 B1 C1 | A2 B2 C2 ...`，同一组里的采样属于同一时刻。A、B、C 的含义和顺序由声道布局决定。这里每个符号代表一个完整采样，不是一个字节；S16 三声道每组占 `3 × 2 = 6` 字节。PCM 可以采用平面或交错排列，“PCM”本身不限定排列方式。

把 FLTP 的第一个平面当成完整双声道数据写出，会丢掉另一个声道；把它直接当成 S16 字节播放，数值解释也会错误。

`AVChannelLayout` 的 `nb_channels` 是声道数量，`order` 和相关布局信息描述各声道的含义。`av_channel_layout_describe()` 可能返回 `stereo`，也可能返回 `1 channels`：后者只说明数量，位置未被明确标记。本例没有自定义混音矩阵；转换器对未标记位置的常见布局可能使用默认映射。

## 4. 音频 AVFrame 存放什么

视频 AVFrame 通常描述一幅图像；音频 AVFrame 描述连续的一批采样。它不是一个采样，也没有统一固定的“每秒多少 AVFrame”。某种编码可能常见每帧 1024 个采样，但不同编码、输入和末尾处理可能不同，应读取实际 nb_samples。

| 成员 | 音频中的含义 |
|---|---|
| `format` | int 类型，存放 `AVSampleFormat` 枚举值；不是视频的 AVPixelFormat |
| `sample_rate` | 每个声道每秒的采样数，单位 Hz |
| `nb_samples` | 这一帧中每个声道的采样数，不是所有声道合计 |
| `ch_layout` | 声道布局，`ch_layout.nb_channels` 为声道数量 |
| `data[i]` | 数据平面指针，如何解释取决于样本格式 |
| `extended_data` | 完整的平面指针数组；平面音频声道较多时，不能只依赖固定大小的 data 数组 |
| `linesize[0]` | 平面格式中一个声道缓冲区的字节容量；交错格式中整个交错缓冲区的字节容量，可能包含对齐填充 |
| `pts` | 这一批采样起始位置的呈现时间戳，可能未知 |
| `best_effort_timestamp` | 解码器根据可用信息推断的呈现时间戳，本例用它输出时间 |

音频通常只使用 linesize[0] 描述平面容量，各平面大小相同；它不是视频中的“相邻两行距离”。写文件时按有效采样数计算长度，不能把全部 linesize 字节直接当作声音数据。

AVFrame 持有采样缓冲区的引用。`av_frame_alloc()` 只创建帧对象，receive 成功才填入解码结果；转换完成后 `av_frame_unref()` 释放本帧引用，对象继续复用。

### data 与 extended_data

`data` 是固定含 8 个指针的数组，`extended_data` 是完整的数据平面指针表。音频平面数量能放进 data 时，extended_data 通常直接指向 data，并不是另一份采样数据；平面格式超过 8 个声道时，需要额外的指针表容纳全部声道。

因此，音频代码统一读取 extended_data 即可，同时支持交错与平面格式。交错音频无论有几个声道，都只有一个数据平面；平面音频通常每个声道一个平面。一个平面不等于一个声道，必须结合 format 判断。

`extended_data[ch]` 的类型是 `uint8_t*`，直接写 `extended_data[ch][n]` 读取的是第 n 个字节。读取 FLTP 的第 ch 个声道、第 n 个采样时，应按 float 解释：

```cpp
const float* channel = reinterpret_cast<const float*>(frame->extended_data[ch]);
float sample = channel[n];  // 仅适用于 FLTP，且 ch、n 在有效范围内。
```

对 FLT 交错格式，则按 float 解释 extended_data[0]，第 n 个时刻、第 ch 个声道的位置为 `n × 声道数 + ch`。

## 5. 采样数、时长与数据量

设 N 为每个声道的采样数、C 为声道数、B 为每个采样的存储字节数、R 为采样率：

```text
音频时长（秒） = N / R
全部声道的采样值数量 = N × C
有效 PCM 字节数 = N × C × B
PCM 每秒字节数 = R × C × B
PCM 每秒比特数 = R × C × B × 8
```

双声道的两个声道同时播放，因此计算时长不能再乘声道数。设备接口有时将“同一时刻各声道的一组采样”称为一个 sample frame；FFmpeg 的一个 AVFrame 可以包含很多组这样的采样。

同一个音频 AVFrame 中，各声道的采样数相同，都等于 nb_samples；不需要分别查询左、右声道的数量。不同 AVFrame 的 nb_samples 则可能不同。

例如 48 kHz、双声道、S16：每秒 `48000 × 2 × 2 = 192000` 字节，PCM 码率为 1536 kbps。AAC 文件设置的 128 kbps 是压缩数据的目标码率，不能据此推算解码后的 PCM 字节数。

本例第一帧 AAC 解码结果：

```text
sample_rate = 44100
nb_samples = 1024
channels = 2
sample_fmt = fltp，单个 float 为 4 字节

本帧时长：1024 / 44100 ≈ 0.023220 秒
每个平面的有效字节：1024 × 4 = 4096
两个平面有效字节合计：8192
实测 linesize[0] = 8192：每个平面有额外容量，不能全部写出
```

PTS 的单位仍由时间基决定。本例设置 `context_->pkt_timebase = stream_->time_base`，以流时间基解释帧时间戳；不能假定任何文件的时间基都恰好等于 `1 / sample_rate`。

## 6. SwrContext 与重采样

`SwrContext` 是 libswresample 的转换上下文，可同时完成三类操作：

| 操作 | 改变什么 | 本例 |
|---|---|---|
| 采样率转换 | 改变每秒的采样数量，重新计算采样值 | 44100 → 48000 |
| 样本格式转换 | 改变数值表示及平面、交错排列 | FLTP → S16 |
| 声道转换 | 按声道含义重排或混合各声道 | 输出固定为 stereo |

SwrContext 不负责解码 AAC，也不负责播放；它接收的已经是 PCM。它与处理视频像素的 SwsContext 是不同类型，创建、转换和释放 API 也不同。

采样率转换会插值、滤波并产生新的采样数，目标是保留原来的时间尺度和音高。把 44.1 kHz 的原始字节直接声明成 48 kHz 播放，会缩短时长、提高音高，这不叫正确重采样。声道转换可能使用混音系数，也不总是简单复制或丢掉某个声道。

转换器需要跨帧保留滤波历史，并可能等待后续采样，因此不能为每个 AVFrame 重建 SwrContext。本例在第一帧到达时，用帧的实际 sample_rate、format、ch_layout 初始化；后续复用，同一流内参数改变时明确报错。

`PrepareResampler()` 只检查参数并准备转换上下文：先确认采样率、采样数、样本格式和布局有效；已有上下文时核对输入参数；首次则复制输入布局，设置目标参数，调用 swr_alloc_set_opts2 和 swr_init。真正读写采样发生在 Convert 中的 swr_convert，不发生在准备函数里。

理论上输入 N 个采样，对应目标数量约为 `N × 输出采样率 / 输入采样率`。这个值可能不是整数，加上内部延迟，一次调用的返回数量并不固定。样本中，前两次输入都是 1024，实际先后输出 1098 和 1114 个采样；持续转换后才能得到完整结果。

## 7. 音频相关 API

### 轨道与解码

| API / 配置 | 作用 |
|---|---|
| `avformat_open_input()` | 打开容器输入 |
| `avformat_find_stream_info()` | 探测并补齐轨道信息 |
| `stream->codecpar->codec_type` | 与 AVMEDIA_TYPE_AUDIO 比较，选择音轨；本例选择第一条 |
| `avcodec_find_decoder(codec_id)` | 按音轨编码查找解码实现 |
| `avcodec_alloc_context3()` | 创建独立的音频解码工作实例 |
| `avcodec_parameters_to_context()` | 复制轨道编码参数和初始化数据 |
| `context->pkt_timebase` | 声明输入包时间戳单位 |
| `avcodec_open2()` | 完成解码器初始化 |
| `av_read_frame()` | 取得一个包，按 stream_index 筛选所选音轨 |
| `avcodec_send_packet()` | 提交编码包；传 nullptr 表示后续没有输入 |
| `avcodec_receive_frame()` | 取得一批 PCM 采样；成功后读取音频 AVFrame 字段 |

音频沿用视频的 send/receive 状态规则：send 返回 EAGAIN 时先取输出，再重送同一个包；receive 返回 EAGAIN 时继续输入；送结束信号后 receive 返回 EOF 才表示解码器排空。包与帧仍不能假定一一对应。

### EAGAIN、内部缓存与外部队列

send 返回 EAGAIN 表示这个包没有被接受，必须先调用 receive 推进输出，再提交同一个包。它不是“另一个取帧线程还没执行完”的通知；这里的 send/receive 在同一线程顺序调用，单纯等待不会改变状态。本例每次送包后都会循环接收，直到需要更多输入，因此正常循环中通常不会触发送包的 EAGAIN 分支。

receive 成功一次只交出一个 AVFrame，但不代表解码器只能保存一帧。AVCodecContext 内部管理输入、解码状态和所需缓存，容量与编码、线程配置和实现有关，没有统一可配置的“最多缓存几帧”。receive 也可能推进解码工作，不能只理解成从已解码队列中取一个元素。

需要提前准备多帧时，由应用建立有上限的队列，并按队列容量控制解码推进。队列必须持有各帧的独立引用或数据：例如用 av_frame_clone 或 av_frame_ref 保留缓冲区引用；它们通常不深拷贝采样。不能反复把同一个 frame_ 指针入队，再 unref 或覆盖它，否则队列不能保留各次结果。

### 格式、布局与转换

| API | 作用 |
|---|---|
| `av_get_sample_fmt_name(format)` | 将格式枚举变为 s16、fltp 等名称，不转换数据 |
| `av_get_bytes_per_sample(format)` | 返回单个声道的一个采样占多少字节 |
| `av_sample_fmt_is_planar(format)` | 判断是否为平面格式 |
| `av_channel_layout_describe()` | 将声道布局描述为文字 |
| `av_channel_layout_check()` | 检查布局描述是否有效 |
| `av_channel_layout_copy()` | 复制布局，包括可能需要独立管理的内部数据 |
| `av_channel_layout_compare()` | 比较两个布局是否一致，0 表示一致 |
| `av_channel_layout_uninit()` | 释放复制的布局所持有的内部资源 |
| `swr_alloc_set_opts2()` | 创建或配置 SwrContext，分别提供输出、输入的布局、格式、采样率 |
| `swr_init()` | 根据已设置的参数初始化转换器 |
| `swr_get_out_samples(swr, N)` | 估算下一次输入 N 个采样时，输出所需容量的上界，包含当前缓存的影响 |
| `swr_convert()` | 执行转换，返回实际输出的每声道采样数，负值为错误 |
| `swr_get_delay(swr, base)` | 查询转换延迟，结果单位为 1/base 秒；本例 base=48000，按输出采样单位记录 |
| `swr_free(&swr)` | 释放转换器及其资源，传入的是指针的地址 |

`swr_alloc_set_opts2()` 的参数顺序是：上下文指针的地址、输出布局/格式/采样率、输入布局/格式/采样率、日志偏移和日志上下文。目标 stereo 布局用 `AV_CHANNEL_LAYOUT_STEREO` 声明。

### swr_convert 的参数、输出位置与声道解释

```cpp
swr_convert(swr, output_planes, output_capacity,
            input_planes, input_samples);
```

| 参数或返回值 | 含义 |
|---|---|
| `swr` | 已初始化的转换上下文，保存输入与输出的采样率、样本格式、声道布局以及转换状态 |
| `output_planes` | 输出平面指针数组；本例交错 S16 只有一个平面 |
| `output_capacity` | 每个声道最多能容纳多少个输出采样，不是字节数 |
| `input_planes` | 输入平面指针数组，本例取 AVFrame::extended_data |
| `input_samples` | 每个声道的输入采样数，本例取 nb_samples |
| 非负返回值 | 每个声道实际生成多少个采样，可能为 0 |

本例用 `vector<int16_t>` 保存输出，分配元素数为 `output_capacity × 2`，写入字节数为 `实际返回数量 × 2 × sizeof(int16_t)`。不能按容量写文件，否则会把无效部分也写进去。转换返回 0 不一定表示结束：正常输入过程中也可能暂时只缓存采样。

`pcm_.resize()` 提供实际输出存储，`output_data = reinterpret_cast<uint8_t*>(pcm_.data())` 只是取得同一块内存的字节指针，不分配或复制采样。swr_convert 将结果写到这块内存后，代码用 `output_.write(pcm_.data() 对应的字节指针, bytes)` 写入 PCM 文件；output_ 是 QFile，不是另一个采样缓冲区。

```text
pcm_.data() ───┐
              ├──→ 同一块输出内存：[L0 R0 L1 R1 ...]
output_data ──┘

swr_convert → 写入这块内存
output_.write → 从这块内存读取有效字节，写入文件
```

参数要求的是平面指针数组。本例只有一个输出平面，所以 `&output_data` 就提供了包含一个平面指针的入口，函数通过 `out[0]` 找到目标缓冲区；它不是让函数替调用方分配输出空间。若输出为平面格式，则需要分别提供各输出平面的指针和足够空间。

swr_convert 的输入指针本身不带“左声道”“float”等标签，也没有接收整个 AVFrame。解释方式来自先前 swr_alloc_set_opts2 保存到上下文中的配置：

| 输入配置 | 指针与数据的解释 |
|---|---|
| FLTP + stereo | extended_data[0] 是左声道 float 序列，extended_data[1] 是右声道 float 序列 |
| FLT + stereo | extended_data[0] 中按 float 读取 L0、R0、L1、R1 等交错序列 |
| S16 + stereo | extended_data[0] 中按 int16_t 读取同样的交错顺序 |

这里的左右对应来自 stereo 布局；其他布局按其声道顺序解释，不能一概认为第一个平面都是左声道。配置描述、实际指针指向的数据、每声道采样数必须一致，转换器不会通过分析声音内容猜测它们。

### 平面输出与 PCM 文件排列

当前代码可以接收双声道 FLTP，输出固定为双声道 S16 交错。只传一个输出平面是由输出格式决定的，不代表只能处理单声道输入。

若输出改为 S16P，须在初始化 SwrContext 前设置该格式，并为每个输出声道提供缓冲区。双声道示意如下，capacity 仍是每声道容量：

```cpp
std::vector<int16_t> left(capacity), right(capacity);
uint8_t* output_planes[] = {
    reinterpret_cast<uint8_t*>(left.data()),
    reinterpret_cast<uint8_t*>(right.data())};
int produced = swr_convert(swr, output_planes, capacity,
                           frame->extended_data, frame->nb_samples);
// produced >= 0 时，每个平面的有效字节数为 produced * sizeof(int16_t)。
```

输入和输出都明确配置为 stereo 时，平面 0 对应左声道，平面 1 对应右声道，不需要每次转换再传声道编号；其他布局按配置中的声道顺序对应或混音。仅有“两个声道”而没有明确布局，不能据此断定它们一定是左、右。

内存中的平面排列不自动规定整个裸 PCM 文件的排列。每次转换后先写左平面再写右平面，会得到 `[本次左][本次右][下次左][下次右]...`，不会得到 `[整段左][整段右]`。前一种写法必须让读取方知道每块长度；若约定整段左声道后接整段右声道，可以分别保存两个声道，结束后顺序拼接。裸 PCM 不记录这些规则，写入方和读取方必须事先约定。

## 8. 程序中的调用关系

```text
Decode
    → 打开输入、探测流、选第一条音轨
    → AudioDecoder 构造：创建并打开解码器、创建 PCM 输出文件
    → 循环 av_read_frame
        → 所选音轨：Send(packet)
            → avcodec_send_packet
            → Receive 循环 avcodec_receive_frame
                → PrepareResampler：首次配置，之后核对并复用
                → 打印音频帧的格式、采样数和时间
                → Convert(frame)
                    → swr_get_out_samples：确定容量
                    → swr_convert：转换 PCM
                    → 写入实际产生的有效字节
                → av_frame_unref，继续接收
        → av_packet_unref，继续读包
    → 输入 EOF：Send(nullptr)，接收剩余解码帧
    → Finish：Convert(nullptr)，取完重采样器尾部
    → 刷新并关闭输出文件，统计采样数与时长
```

本例 `context_` 保存音频解码状态，`frame_` 反复接收 PCM 帧，`resampler_` 保存转换状态，`pcm_` 是转换结果的临时存储。写文件后才复用 pcm_；解码结果转换完成后才 unref。

Qt Core 的 QCoreApplication 用于取得命令行参数，QFile 负责 Unicode 路径和排他写文件；没有调用 app.exec，也没有 Qt 窗口。FFmpeg 的错误码经 av_strerror 转成错误说明，由异常处理统一输出。

## 9. 两次排空与音频时长

```text
输入文件读完
    → avcodec_send_packet(context, nullptr)
    → receive_frame 取到 AVERROR_EOF
    → swr_convert(swr, out, capacity, nullptr, 0)
    → 重复空输入转换，直到返回 0
```

第一步取出解码器内部尚未交出的 PCM，第二步取出重采样器尚未交出的目标 PCM。释放上下文只能释放资源，不能代替这两次取尾部数据的操作。

本样本重采样结束时还输出 17 个采样。空输入转换返回 0 才是本例停止取尾部的条件，不能用 swr_get_delay 是否为 0 判断；实测最后 delay_samples 仍可能非零。

AAC 等编码按块处理，可能有起始延迟和末尾补齐；容器时间戳、包时长及跳过采样等信息共同描述播放范围。不能把编码包数量乘一个固定帧大小就当成精确播放时长。

本样本首个音频包 PTS 为 -1024，并带跳过 1024 个采样的信息；解码器处理后，首个输出帧 PTS 为 0。末包标记时长为 340 个时间单位，但最后的解码帧仍有每声道 1024 个采样。本例连续保存实际输出，不按容器五秒时长裁切，最终 PCM 约为 5.015521 秒；这不是播放时钟累计误差。

## 10. 本例数据与设备播放的边界

| 位置 | 实际数据 |
|---|---|
| 带音轨的 MP4 副本 | 原 H.264 视频 + 44.1 kHz stereo AAC；测试音左 440 Hz、右 880 Hz |
| AAC 解码输出 | FLTP，共 216 个 AVFrame，每声道合计 221184 个采样 |
| 转换后的 PCM | 48 kHz、stereo、S16 小端交错，每声道 240745 个采样 |
| PCM 文件体积 | 962980 字节，即 240745 × 2 × 2 |

裸 PCM 只包含采样字节，不自带采样率、声道、格式、时间戳或文件头，读取方必须知道这些参数。给文件改名为 .wav 不会生成 WAV；WAV 是容器，需要写入描述音频参数的头部。本例的 WAV 试听副本由 FFmpeg 命令行将导出的 PCM 封装得到。

真正输出到设备时，需要选择设备支持的格式，并处理设备缓冲。已经解码、已经交给设备、设备实际播放到哪里是三个不同进度；不能用“已写入多少字节”直接当成已经听到的时长。这些信息会成为音频播放及第五阶段音画同步的基础。
