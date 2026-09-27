#include "common/VideoFrameDecoder.h"

#include "log/colorful-log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

namespace
{

/// @brief 测试视频宽度。
/// @note 极小尺寸降低编码成本，同时保留 YUV420 偶数尺寸要求。
constexpr int TEST_VIDEO_WIDTH = 16;

/// @brief 测试视频高度。
/// @note 与宽度相同，方便按首像素比较各帧亮度。
constexpr int TEST_VIDEO_HEIGHT = 16;

/// @brief 测试视频帧率。
/// @note 每帧间隔 0.5 秒，便于构造顺序、跳转和回退请求。
constexpr int TEST_VIDEO_FPS = 2;

/// @brief 测试视频总帧数。
/// @note 六帧覆盖约三秒时间线，并包含超过一秒的 Seek 跳转。
constexpr int TEST_VIDEO_FRAME_COUNT = 6;

/// @brief 释放测试编码器上下文。
struct CodecContextDeleter {
    /// @brief 释放编码器并清空临时指针。
    /// @param context 由 avcodec_alloc_context3 分配的上下文；允许为空。
    void operator()(AVCodecContext* context) const
    {
        // 测试提前失败时仍由 unique_ptr 自动释放编码器状态。
        if ( context ) avcodec_free_context(&context);
    }
};

/// @brief 释放测试编码帧。
struct FrameDeleter {
    /// @brief 释放帧及其引用缓冲。
    /// @param frame 由 av_frame_alloc 分配的测试帧；允许为空。
    void operator()(AVFrame* frame) const
    {
        // av_frame_free 同时释放 av_frame_get_buffer 建立的引用缓冲。
        if ( frame ) av_frame_free(&frame);
    }
};

/// @brief 释放测试编码包。
struct PacketDeleter {
    /// @brief 释放包及其引用缓冲。
    /// @param packet 由 av_packet_alloc 分配的编码包；允许为空。
    void operator()(AVPacket* packet) const
    {
        // 提前返回时仍释放最后一个尚未写出的包引用。
        if ( packet ) av_packet_free(&packet);
    }
};

/// @brief 关闭输出文件并释放容器上下文。
struct OutputContextDeleter {
    /// @brief 关闭 AVIO 后释放输出容器。
    /// @param context 由 avformat_alloc_output_context2 创建的上下文。
    void operator()(AVFormatContext* context) const
    {
        // 分配输出上下文失败时允许空指针直接返回。
        if ( !context ) return;
        // 只有实际打开过 AVIO 的容器才需要关闭文件句柄。
        if ( context->pb ) avio_closep(&context->pb);
        // 容器结构最后释放，其中的流和 codecpar 由 FFmpeg 一并管理。
        avformat_free_context(context);
    }
};

/// @brief 测试编码器上下文的独占 RAII 句柄。
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
/// @brief 测试源帧的独占 RAII 句柄。
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
/// @brief 编码输出包的独占 RAII 句柄。
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
/// @brief 输出容器和 AVIO 文件的独占 RAII 句柄。
using OutputContextPtr = std::unique_ptr<AVFormatContext, OutputContextDeleter>;

/// @brief 接收编码器当前可用数据包并写入容器。
/// @param codecContext 已打开的视频编码器。
/// @param formatContext 已写入头部的输出容器。
/// @param stream 目标视频流。
/// @param packet 复用输出数据包。
/// @return 所有已产生数据包写入成功时返回 true。
/// @note EAGAIN 表示编码器当前需要更多输入帧，EOF 表示 drain 已完成。
bool writeAvailablePackets(AVCodecContext&  codecContext,
                           AVFormatContext& formatContext, AVStream& stream,
                           AVPacket& packet)
{
    // 一次输入帧可能产生零个或多个包，循环直到编码器暂时无输出。
    while ( true ) {
        // packet 在每次成功写出后解除引用，并由下一次 receive 复用。
        const int receiveResult =
            avcodec_receive_packet(&codecContext, &packet);
        if ( receiveResult == AVERROR(EAGAIN) ||
             receiveResult == AVERROR_EOF ) {
            // 两种状态都表示当前已取完所有可写包，并非测试失败。
            return true;
        }
        // 其他负值表示编码阶段不可恢复错误。
        if ( receiveResult < 0 ) return false;

        // 编码器和容器流可能使用不同时间基，写入前统一重采样时间戳。
        av_packet_rescale_ts(&packet, codecContext.time_base, stream.time_base);
        // 单流测试容器仍显式标记目标流索引，符合 muxer 输入契约。
        packet.stream_index = stream.index;
        const int writeResult =
            av_interleaved_write_frame(&formatContext, &packet);
        // muxer 已取得包内容后立即解除引用，无论写入是否成功。
        av_packet_unref(&packet);
        // 写入失败终止 fixture 生成，避免测试使用不完整容器。
        if ( writeResult < 0 ) return false;
    }
}

/// @brief 填充一帧可明显区分亮度的 YUV420P 测试图像。
/// @param frame 已分配图像缓冲的帧。
/// @param frameIndex 当前帧序号。
void fillTestFrame(AVFrame& frame, int frameIndex)
{
    // 每帧 Y 分量递增 35，保证解码后亮度顺序具有明显差异。
    const std::uint8_t luma = static_cast<std::uint8_t>(32 + frameIndex * 35);
    // 按 FFmpeg 行跨度填写有效宽度，不覆盖可能存在的行尾对齐字节。
    for ( int y = 0; y < frame.height; ++y ) {
        std::fill_n(frame.data[0] + y * frame.linesize[0], frame.width, luma);
    }

    // YUV420 色度平面横纵分辨率均为亮度平面的一半并向上取整。
    const int chromaWidth  = (frame.width + 1) / 2;
    const int chromaHeight = (frame.height + 1) / 2;
    for ( int y = 0; y < chromaHeight; ++y ) {
        // U/V 固定为中性 128，使输出颜色只由亮度分量决定。
        std::fill_n(frame.data[1] + y * frame.linesize[1],
                    chromaWidth,
                    std::uint8_t{ 128 });
        std::fill_n(frame.data[2] + y * frame.linesize[2],
                    chromaWidth,
                    std::uint8_t{ 128 });
    }
}

/// @brief 填充一帧可区分 BT.709 与默认 BT.601 矩阵的 YUV420P 图像。
/// @param frame 已分配图像缓冲的帧。
void fillBt709ColorFrame(AVFrame& frame)
{
    // 固定 Y=100，并使用非中性色度，放大 BT.709 与 BT.601 矩阵差异。
    for ( int y = 0; y < frame.height; ++y ) {
        std::fill_n(frame.data[0] + y * frame.linesize[0], frame.width, 100);
    }

    // 色度平面仍遵循 YUV420 的二分辨率布局。
    const int chromaWidth  = (frame.width + 1) / 2;
    const int chromaHeight = (frame.height + 1) / 2;
    for ( int y = 0; y < chromaHeight; ++y ) {
        // U/V 同设为 200，得到可稳定断言的紫红色 RGBA 输出。
        std::fill_n(frame.data[1] + y * frame.linesize[1], chromaWidth, 200);
        std::fill_n(frame.data[2] + y * frame.linesize[2], chromaWidth, 200);
    }
}

/// @brief 使用当前预编译 FFmpeg 在测试输出目录生成极小 AVI/MJPEG。
/// @param filePath 输出文件路径。
/// @return 完整写入容器时返回 true。
/// @note fixture 在运行时生成，避免提交依赖特定编码器版本的二进制视频。
bool createTestVideo(const std::filesystem::path& filePath)
{
    // 函数以布尔值汇总所有 FFmpeg 步骤，任一失败都由 RAII 回滚已建资源。
    // 输出格式显式指定 AVI，扩展名只承担可读文件名作用。
    AVFormatContext*  rawFormatContext{ nullptr };
    const std::string path = filePath.string();
    if ( avformat_alloc_output_context2(
             &rawFormatContext, nullptr, "avi", path.c_str()) < 0 ||
         !rawFormatContext ) {
        // muxer 不可用或上下文未创建时无法生成后续解码输入。
        return false;
    }
    // 从此处开始由 RAII 负责关闭 AVIO 并释放容器。
    OutputContextPtr formatContext(rawFormatContext);

    // MJPEG 为逐帧内编码，适合验证大跨度和倒退 Seek 的关键帧行为。
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if ( !encoder ) return false;

    // 单一视频流足以覆盖解码器的最佳视频流选择。
    AVStream* stream = avformat_new_stream(formatContext.get(), nullptr);
    if ( !stream ) return false;

    // 编码上下文先局部配置，所有失败路径都保持测试目录中产物不可用。
    CodecContextPtr codecContext(avcodec_alloc_context3(encoder));
    if ( !codecContext ) return false;

    // 编码器参数固定为 16x16、2 FPS 的全范围 YUV420 MJPEG。
    codecContext->codec_id   = AV_CODEC_ID_MJPEG;
    codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
    codecContext->width      = TEST_VIDEO_WIDTH;
    codecContext->height     = TEST_VIDEO_HEIGHT;
    // YUVJ420P 显式表达 JPEG 全范围，与下方 color_range 声明一致。
    codecContext->pix_fmt   = AV_PIX_FMT_YUVJ420P;
    codecContext->time_base = AVRational{ 1, TEST_VIDEO_FPS };
    // framerate 与 time_base 互为倒数，使帧 PTS 可以直接使用序号。
    codecContext->framerate   = AVRational{ TEST_VIDEO_FPS, 1 };
    codecContext->color_range = AVCOL_RANGE_JPEG;
    if ( formatContext->oformat->flags & AVFMT_GLOBALHEADER ) {
        // 仅在容器要求时把编解码器头写入全局 extradata。
        codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // 打开编码器后其最终像素格式和时间基才可复制到流参数。
    if ( avcodec_open2(codecContext.get(), encoder, nullptr) < 0 ) return false;

    stream->time_base = codecContext->time_base;
    // 容器流必须携带编码器参数，解码端才能重建视频上下文。
    if ( avcodec_parameters_from_context(stream->codecpar, codecContext.get()) <
         0 ) {
        return false;
    }

    // 需要外部文件的 muxer 显式打开 AVIO；内存或特殊 muxer 可跳过。
    if ( !(formatContext->oformat->flags & AVFMT_NOFILE) &&
         avio_open(&formatContext->pb, path.c_str(), AVIO_FLAG_WRITE) < 0 ) {
        return false;
    }
    // 容器头写入成功后才允许提交编码数据包。
    if ( avformat_write_header(formatContext.get(), nullptr) < 0 ) return false;

    // 帧和包在全部六次编码中复用，避免测试循环反复分配结构。
    FramePtr  frame(av_frame_alloc());
    PacketPtr packet(av_packet_alloc());
    if ( !frame || !packet ) return false;

    // 帧描述与已打开编码器的最终格式和尺寸保持一致。
    frame->format = codecContext->pix_fmt;
    frame->width  = codecContext->width;
    frame->height = codecContext->height;
    // 32 字节对齐满足常见 SIMD 编码路径，同时由 AVFrame 管理缓冲。
    if ( av_frame_get_buffer(frame.get(), 32) < 0 ) return false;

    // 逐帧写入递增亮度图像，PTS 等于帧序号。
    for ( int frameIndex = 0; frameIndex < TEST_VIDEO_FRAME_COUNT;
          ++frameIndex ) {
        // 编码器可能仍持有上一帧引用，写入前请求可写的独立缓冲。
        if ( av_frame_make_writable(frame.get()) < 0 ) return false;
        fillTestFrame(*frame, frameIndex);
        // 连续整数 PTS 在 1/FPS 时间基下对应精确的半秒帧间隔。
        frame->pts = frameIndex;
        // 每次送帧后立即排出当前可用包，防止编码器输出队列积压。
        if ( avcodec_send_frame(codecContext.get(), frame.get()) < 0 ||
             !writeAvailablePackets(
                 *codecContext, *formatContext, *stream, *packet) ) {
            return false;
        }
    }

    // 空帧触发编码器 drain，写出最后可能延迟的编码包。
    if ( avcodec_send_frame(codecContext.get(), nullptr) < 0 ||
         !writeAvailablePackets(
             *codecContext, *formatContext, *stream, *packet) ) {
        return false;
    }

    // trailer 成功表示索引和容器尾部完整，可供后续 Seek 测试使用。
    return av_write_trailer(formatContext.get()) >= 0;
}

/// @brief 生成带 BT.709 limited-range 标记的 Matroska/MPEG-4 视频。
/// @param filePath 输出文件路径。
/// @return 完整写入容器时返回 true。
/// @note 单帧 fixture 专门验证帧级色彩属性是否传入 swscale。
bool createBt709ColorVideo(const std::filesystem::path& filePath)
{
    // 与普通 fixture 分开实现，避免色彩专用参数改变时序测试编码特征。
    // Matroska 能稳定保存 MPEG-4 视频及显式色彩元数据。
    AVFormatContext*  rawFormatContext{ nullptr };
    const std::string path = filePath.string();
    if ( avformat_alloc_output_context2(
             &rawFormatContext, nullptr, "matroska", path.c_str()) < 0 ||
         !rawFormatContext ) {
        // 当前预编译 FFmpeg 缺少 muxer 时记录明确环境原因。
        XERROR("VideoFrameDecoderTest: Matroska muxer is unavailable");
        return false;
    }
    // 容器从创建成功起由 RAII 负责关闭输出句柄和释放流。
    OutputContextPtr formatContext(rawFormatContext);

    // MPEG-4 编码器支持 YUV420P limited-range 与 BT.709 标记。
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if ( !encoder ) {
        XERROR("VideoFrameDecoderTest: MPEG-4 encoder is unavailable");
        return false;
    }

    // fixture 仍只创建一个视频流，排除最佳流选择歧义。
    AVStream* stream = avformat_new_stream(formatContext.get(), nullptr);
    if ( !stream ) return false;

    // 独立编码上下文保存 fixture 的全部颜色和 GOP 契约。
    CodecContextPtr codecContext(avcodec_alloc_context3(encoder));
    if ( !codecContext ) return false;

    // 一秒一帧、单关键帧且禁用 B 帧，保证单帧输出时间戳确定。
    codecContext->codec_id   = AV_CODEC_ID_MPEG4;
    codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
    codecContext->width      = TEST_VIDEO_WIDTH;
    codecContext->height     = TEST_VIDEO_HEIGHT;
    // 标准 YUV420P 配合 MPEG limited range，区别于 MJPEG 全范围 fixture。
    codecContext->pix_fmt   = AV_PIX_FMT_YUV420P;
    codecContext->time_base = AVRational{ 1, 1 };
    codecContext->framerate = AVRational{ 1, 1 };
    // 较高码率减少单帧色彩量化误差，使矩阵断言跨平台保持稳定。
    codecContext->bit_rate = 1'000'000;
    codecContext->gop_size = 1;
    // 禁用 B 帧保证送入单帧后 drain 即可取得全部输出。
    codecContext->max_b_frames = 0;
    // limited-range BT.709 属性必须同时写入编码上下文和测试源帧。
    codecContext->color_range     = AVCOL_RANGE_MPEG;
    codecContext->colorspace      = AVCOL_SPC_BT709;
    codecContext->color_primaries = AVCOL_PRI_BT709;
    codecContext->color_trc       = AVCOL_TRC_BT709;
    if ( formatContext->oformat->flags & AVFMT_GLOBALHEADER ) {
        // 遵循 Matroska muxer 的全局头要求，不假设固定 flags。
        codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // 编码器初始化失败时不继续生成不可解码的伪 fixture。
    if ( avcodec_open2(codecContext.get(), encoder, nullptr) < 0 ) return false;

    // 流时间基和 codecpar 从最终编码器上下文同步到容器。
    stream->time_base = codecContext->time_base;
    if ( avcodec_parameters_from_context(stream->codecpar, codecContext.get()) <
         0 ) {
        return false;
    }

    // 普通文件 muxer 需要打开写入 AVIO，然后写入容器头。
    if ( !(formatContext->oformat->flags & AVFMT_NOFILE) &&
         avio_open(&formatContext->pb, path.c_str(), AVIO_FLAG_WRITE) < 0 ) {
        return false;
    }
    if ( avformat_write_header(formatContext.get(), nullptr) < 0 ) return false;

    // 单帧仍复用与多帧 fixture 相同的 RAII 帧和包结构。
    FramePtr  frame(av_frame_alloc());
    PacketPtr packet(av_packet_alloc());
    if ( !frame || !packet ) return false;

    // 帧的格式、尺寸和四项色彩属性与编码器声明严格一致。
    frame->format = codecContext->pix_fmt;
    frame->width  = codecContext->width;
    frame->height = codecContext->height;
    // 帧级范围和矩阵是被测转换路径实际读取的权威色彩属性。
    frame->color_range     = codecContext->color_range;
    frame->colorspace      = codecContext->colorspace;
    frame->color_primaries = codecContext->color_primaries;
    frame->color_trc       = codecContext->color_trc;
    // 分配并确认帧缓冲可写后才能填入测试 YUV 分量。
    if ( av_frame_get_buffer(frame.get(), 32) < 0 ||
         av_frame_make_writable(frame.get()) < 0 ) {
        return false;
    }

    // 固定颜色帧置于时间零点，方便解码测试直接请求 0.0 秒。
    fillBt709ColorFrame(*frame);
    frame->pts = 0;
    // 依次发送真实帧、排出包、发送 drain 并排出剩余包。
    if ( avcodec_send_frame(codecContext.get(), frame.get()) < 0 ||
         !writeAvailablePackets(
             *codecContext, *formatContext, *stream, *packet) ||
         avcodec_send_frame(codecContext.get(), nullptr) < 0 ||
         !writeAvailablePackets(
             *codecContext, *formatContext, *stream, *packet) ) {
        return false;
    }

    // 完整 trailer 确保容器色彩元数据和索引已经落盘。
    return av_write_trailer(formatContext.get()) >= 0;
}

/// @brief 验证探测、顺序解码、大跨度 Seek 与倒退 Seek。
/// @param outputDirectory 测试生成文件目录。
/// @return 所有行为符合预期时返回 true。
/// @note 测试使用运行时生成视频，不依赖源码树中的二进制资源。
bool testVideoDecode(const std::filesystem::path& outputDirectory)
{
    // 所有生成文件写入 CMake 传入的 test_output 子目录。
    std::error_code filesystemError;
    std::filesystem::create_directories(outputDirectory, filesystemError);
    // 目录创建失败时无法安全生成 fixture，立即报告失败。
    if ( filesystemError ) return false;

    // 固定文件名便于失败后人工检查，生成前移除上次遗留产物。
    const std::filesystem::path videoPath = outputDirectory / "tiny_mjpeg.avi";
    std::filesystem::remove(videoPath, filesystemError);
    // 不存在文件导致的 remove 状态不影响本次重新生成。
    filesystemError.clear();
    // fixture 必须完整写出头、帧和 trailer 后才进入被测逻辑。
    if ( !createTestVideo(videoPath) ) return false;

    // 独立探测入口应返回编码尺寸和正容器时长。
    const auto probed = MMM::Utils::probeVideoInfo(videoPath);
    if ( !probed || probed->width != TEST_VIDEO_WIDTH ||
         probed->height != TEST_VIDEO_HEIGHT || probed->duration <= 0.0 ) {
        // 探测结果任一字段不满足 fixture 契约都停止后续帧测试。
        return false;
    }

    // 正式解码器打开同一文件，验证状态和公开信息与探测结果一致。
    MMM::Utils::VideoFrameDecoder decoder;
    if ( !decoder.open(videoPath) || !decoder.isOpen() ||
         decoder.info().width != TEST_VIDEO_WIDTH ||
         decoder.info().height != TEST_VIDEO_HEIGHT ) {
        // open、isOpen 和 info 必须对同一初始化状态给出一致结果。
        return false;
    }

    // 每份选中帧按值复制，避免后续 decodeFrameAt 覆盖内部观察指针。
    MMM::Utils::VideoFrame firstFrame;
    MMM::Utils::VideoFrame middleFrame;
    MMM::Utils::VideoFrame jumpedFrame;
    MMM::Utils::VideoFrame rewoundFrame;
    MMM::Utils::VideoFrame endFrame;
    // 首次请求视频起点，建立顺序解码缓存和首帧基线。
    const auto* decodedFrame = decoder.decodeFrameAt(0.0);
    if ( !decodedFrame ) return false;
    // 保存内部 vector 地址，用于验证近邻请求会复用同一当前帧对象。
    const std::uint8_t* firstFramePixels = decodedFrame->rgba.data();
    firstFrame                           = *decodedFrame;
    decodedFrame                         = decoder.decodeFrameAt(0.1);
    // 0.1 秒仍早于第二帧，应返回相同时间戳且不重分配当前像素缓冲。
    if ( !decodedFrame || decodedFrame->timestamp != firstFrame.timestamp ||
         decodedFrame->rgba.data() != firstFramePixels ) {
        return false;
    }
    // 0.7 秒顺序向前推进，应选取不晚于目标的中间帧。
    decodedFrame = decoder.decodeFrameAt(0.7);
    if ( !decodedFrame ) return false;
    middleFrame = *decodedFrame;
    // 从 0.7 跳到 2.1 秒超过阈值，覆盖大跨度正向 Seek 路径。
    decodedFrame = decoder.decodeFrameAt(2.1);
    if ( !decodedFrame ) return false;
    jumpedFrame = *decodedFrame;
    // 随后请求 0.0 秒强制倒退 Seek，并应恢复首帧内容。
    decodedFrame = decoder.decodeFrameAt(0.0);
    if ( !decodedFrame ) return false;
    rewoundFrame = *decodedFrame;
    // 超过总时长的请求应钳制到结尾前并返回最后可显示帧。
    decodedFrame = decoder.decodeFrameAt(probed->duration + 10.0);
    if ( !decodedFrame ) return false;
    endFrame = *decodedFrame;

    // RGBA8 紧密布局要求每帧恰好包含 width * height * 4 字节。
    const std::size_t expectedBytes =
        static_cast<std::size_t>(TEST_VIDEO_WIDTH) * TEST_VIDEO_HEIGHT * 4U;
    if ( firstFrame.rgba.size() != expectedBytes ||
         middleFrame.rgba.size() != expectedBytes ||
         jumpedFrame.rgba.size() != expectedBytes ||
         rewoundFrame.rgba.size() != expectedBytes ||
         endFrame.rgba.size() != expectedBytes ) {
        // 任一帧尺寸不一致都表示像素转换或帧复制契约破坏。
        return false;
    }
    // 帧选择必须不晚于目标，并在倒退和结尾请求中保持时间顺序。
    if ( firstFrame.timestamp > 0.01 || middleFrame.timestamp > 0.7 ||
         jumpedFrame.timestamp > 2.1 || rewoundFrame.timestamp > 0.01 ||
         endFrame.timestamp > probed->duration ||
         endFrame.timestamp < jumpedFrame.timestamp ) {
        // 该组断言覆盖不晚于目标、倒退恢复和 EOF 钳制三项选帧规则。
        return false;
    }
    // 递增亮度 fixture 使跳转帧首通道更亮，倒退帧则应恢复首帧亮度。
    if ( jumpedFrame.rgba[0] <= firstFrame.rgba[0] ||
         rewoundFrame.rgba[0] != firstFrame.rgba[0] ) {
        return false;
    }

    // 非有限目标时间必须在进入 FFmpeg 前被拒绝。
    const double nan = std::numeric_limits<double>::quiet_NaN();
    if ( decoder.decodeFrameAt(nan) ) return false;

    // 显式关闭后状态应为未打开，公开尺寸恢复为零。
    decoder.close();
    return !decoder.isOpen() && decoder.info().width == 0 &&
           decoder.info().height == 0;
}

/// @brief 验证不存在的路径不会返回伪造视频信息。
/// @param outputDirectory 测试输出目录。
/// @return 无效路径被拒绝时返回 true。
bool testInvalidPath(const std::filesystem::path& outputDirectory)
{
    // 不创建目标文件，探测必须返回空 optional 而非伪造全零 VideoInfo。
    return !MMM::Utils::probeVideoInfo(outputDirectory / "missing-video.avi");
}

/// @brief 验证帧级色彩属性会驱动 BT.709 到 RGBA 的转换矩阵。
/// @param outputDirectory 测试输出目录。
/// @return 输出像素符合 BT.709 limited-range 转换时返回 true。
/// @note 通道断言保留小幅编码误差容差，但足以区分错误矩阵。
bool testBt709ColorConversion(const std::filesystem::path& outputDirectory)
{
    // 色彩 fixture 使用独立文件，避免与时序测试的 MJPEG 容器互相影响。
    const std::filesystem::path videoPath =
        outputDirectory / "bt709_color_mpeg4.mkv";
    std::error_code filesystemError;
    // 清除上次运行产物，确保本次编码器色彩元数据真实写入。
    std::filesystem::remove(videoPath, filesystemError);
    if ( !createBt709ColorVideo(videoPath) ) {
        XERROR("VideoFrameDecoderTest: Failed to create BT.709 fixture");
        return false;
    }

    // 被测解码器打开 fixture 并请求唯一的零点视频帧。
    MMM::Utils::VideoFrameDecoder decoder;
    if ( !decoder.open(videoPath) ) return false;
    const MMM::Utils::VideoFrame* frame = decoder.decodeFrameAt(0.0);
    // 至少四字节才能安全读取首像素完整 RGBA 四通道。
    if ( !frame || frame->rgba.size() < 4 ) return false;

    // MPEG-4 有损编码允许每通道五级误差，Alpha 必须保持精确不透明。
    const auto channelNear = [](std::uint8_t value, int expected) {
        return std::abs(static_cast<int>(value) - expected) <= 5;
    };
    // 期望值来自 limited-range BT.709 对固定 YUV(100,200,200) 的转换。
    const bool convertedWithBt709 =
        channelNear(frame->rgba[0], 226) && channelNear(frame->rgba[1], 42) &&
        channelNear(frame->rgba[2], 249) && frame->rgba[3] == 255;
    if ( !convertedWithBt709 ) {
        // 失败日志输出实际首像素，便于判断矩阵、范围或通道顺序问题。
        XERROR("VideoFrameDecoderTest: Unexpected BT.709 RGBA {},{},{},{}",
               frame->rgba[0],
               frame->rgba[1],
               frame->rgba[2],
               frame->rgba[3]);
    }
    return convertedWithBt709;
}

/// @brief 在设置探针环境变量时验证外部真实视频。
/// @return 未设置探针时直接通过；设置时必须成功探测并解码。
/// @note 该可选入口用于诊断真实编码格式，不影响默认自包含回归测试。
bool testExternalProbeFile()
{
    // 环境变量只读取路径，不修改测试或个人配置状态。
    const char* probePath = std::getenv("MMM_VIDEO_PROBE_FILE");
    if ( !probePath || probePath[0] == '\0' ) {
        // 未请求外部探针时跳过该场景并保持测试成功。
        return true;
    }

    // 外部路径按平台 filesystem 语义解析，再复用正式探测入口。
    const std::filesystem::path videoPath(probePath);
    const auto                  info = MMM::Utils::probeVideoInfo(videoPath);
    if ( !info || info->width == 0 || info->height == 0 ) {
        // 真实文件必须至少提供正尺寸，未知时长仍允许继续解码零点。
        XERROR("VideoFrameDecoderTest: Failed to probe external video {}",
               videoPath.string());
        return false;
    }

    // 有时长视频选择一秒或中点中较早者，无时长视频从零点开始。
    MMM::Utils::VideoFrameDecoder decoder;
    const double                  targetTime =
        info->duration > 0.0 ? std::min(1.0, info->duration * 0.5) : 0.0;
    if ( !decoder.open(videoPath) ) {
        // 探测成功但正式打开失败同样属于解码器状态不一致。
        XERROR("VideoFrameDecoderTest: Failed to open external video {}",
               videoPath.string());
        return false;
    }
    // 首次时间点请求验证 RGBA 数据与探测尺寸保持一致。
    const MMM::Utils::VideoFrame* frame = decoder.decodeFrameAt(targetTime);
    if ( !frame || frame->rgba.empty() || frame->width != info->width ||
         frame->height != info->height ) {
        XERROR("VideoFrameDecoderTest: Failed to decode external video {}",
               videoPath.string());
        return false;
    }

    /// @brief 模拟暂停状态下连续拖动进度条产生的前后跳转序列。
    /// @note 非单调比例同时覆盖倒退 Seek 和多次大跨度正向 Seek。
    constexpr std::array<double, 12> scrubFractions = {
        0.83, 0.12, 0.68, 0.24, 0.91, 0.37, 0.55, 0.08, 0.76, 0.43, 0.97, 0.31,
    };
    // 只有容器提供正时长时才能把比例安全换算为有效时间点。
    if ( info->duration > 0.0 ) {
        // 每次 decodeFrameAt 返回的指针只在下一次调用前读取和验证。
        for ( const double fraction : scrubFractions ) {
            // 每个比例都位于开区间，避免精确 EOF 的容器差异。
            const double scrubTime = info->duration * fraction;
            frame                  = decoder.decodeFrameAt(scrubTime);
            if ( !frame || frame->rgba.empty() || frame->width != info->width ||
                 frame->height != info->height ) {
                // 任一拖动点失败都输出文件和目标秒数，便于复现特定 Seek。
                XERROR(
                    "VideoFrameDecoderTest: Scrub decode failed for {} at "
                    "{:.3f}s",
                    videoPath.string(),
                    scrubTime);
                return false;
            }
        }
    }

    // 成功日志保留外部文件、尺寸和最后帧时间，作为手动探针证据。
    XINFO(
        "VideoFrameDecoderTest: External probe passed for {} [{}x{}, "
        "frame {:.3f}s]",
        videoPath.string(),
        frame->width,
        frame->height,
        frame->timestamp);
    return true;
}

}  // namespace

/// @brief 覆盖 FFmpeg 视频探测、RGBA 转换与时间点帧选择。
/// @param argc 命令行参数数量。
/// @param argv 第一个参数为测试输出目录。
/// @return 所有检查通过时返回 0。
int main(int argc, char** argv)
{
    // CMake 测试注册必须传入隔离输出目录，缺失时拒绝写入当前目录。
    if ( argc < 2 ) {
        XERROR("VideoFrameDecoderTest: Missing output directory");
        return 1;
    }

    // 四组场景短路串联，任一失败统一以非零退出码报告给 CTest。
    const std::filesystem::path outputDirectory(argv[1]);
    return testVideoDecode(outputDirectory) &&
                   testInvalidPath(outputDirectory) &&
                   testBt709ColorConversion(outputDirectory) &&
                   testExternalProbeFile()
               ? 0
               : 1;
}
