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
constexpr int TEST_VIDEO_WIDTH = 16;

/// @brief 测试视频高度。
constexpr int TEST_VIDEO_HEIGHT = 16;

/// @brief 测试视频帧率。
constexpr int TEST_VIDEO_FPS = 2;

/// @brief 测试视频总帧数。
constexpr int TEST_VIDEO_FRAME_COUNT = 6;

/// @brief 释放测试编码器上下文。
struct CodecContextDeleter {
    /// @brief 释放编码器并清空临时指针。
    void operator()(AVCodecContext* context) const
    {
        if ( context ) avcodec_free_context(&context);
    }
};

/// @brief 释放测试编码帧。
struct FrameDeleter {
    /// @brief 释放帧及其引用缓冲。
    void operator()(AVFrame* frame) const
    {
        if ( frame ) av_frame_free(&frame);
    }
};

/// @brief 释放测试编码包。
struct PacketDeleter {
    /// @brief 释放包及其引用缓冲。
    void operator()(AVPacket* packet) const
    {
        if ( packet ) av_packet_free(&packet);
    }
};

/// @brief 关闭输出文件并释放容器上下文。
struct OutputContextDeleter {
    /// @brief 关闭 AVIO 后释放输出容器。
    void operator()(AVFormatContext* context) const
    {
        if ( !context ) return;
        if ( context->pb ) avio_closep(&context->pb);
        avformat_free_context(context);
    }
};

using CodecContextPtr  = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr         = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr        = std::unique_ptr<AVPacket, PacketDeleter>;
using OutputContextPtr = std::unique_ptr<AVFormatContext, OutputContextDeleter>;

/// @brief 接收编码器当前可用数据包并写入容器。
/// @param codecContext 已打开的视频编码器。
/// @param formatContext 已写入头部的输出容器。
/// @param stream 目标视频流。
/// @param packet 复用输出数据包。
/// @return 所有已产生数据包写入成功时返回 true。
bool writeAvailablePackets(AVCodecContext&  codecContext,
                           AVFormatContext& formatContext, AVStream& stream,
                           AVPacket& packet)
{
    while ( true ) {
        const int receiveResult =
            avcodec_receive_packet(&codecContext, &packet);
        if ( receiveResult == AVERROR(EAGAIN) ||
             receiveResult == AVERROR_EOF ) {
            return true;
        }
        if ( receiveResult < 0 ) return false;

        av_packet_rescale_ts(&packet, codecContext.time_base, stream.time_base);
        packet.stream_index = stream.index;
        const int writeResult =
            av_interleaved_write_frame(&formatContext, &packet);
        av_packet_unref(&packet);
        if ( writeResult < 0 ) return false;
    }
}

/// @brief 填充一帧可明显区分亮度的 YUV420P 测试图像。
/// @param frame 已分配图像缓冲的帧。
/// @param frameIndex 当前帧序号。
void fillTestFrame(AVFrame& frame, int frameIndex)
{
    const std::uint8_t luma = static_cast<std::uint8_t>(32 + frameIndex * 35);
    for ( int y = 0; y < frame.height; ++y ) {
        std::fill_n(frame.data[0] + y * frame.linesize[0], frame.width, luma);
    }

    const int chromaWidth  = (frame.width + 1) / 2;
    const int chromaHeight = (frame.height + 1) / 2;
    for ( int y = 0; y < chromaHeight; ++y ) {
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
    for ( int y = 0; y < frame.height; ++y ) {
        std::fill_n(frame.data[0] + y * frame.linesize[0], frame.width, 100);
    }

    const int chromaWidth  = (frame.width + 1) / 2;
    const int chromaHeight = (frame.height + 1) / 2;
    for ( int y = 0; y < chromaHeight; ++y ) {
        std::fill_n(frame.data[1] + y * frame.linesize[1], chromaWidth, 200);
        std::fill_n(frame.data[2] + y * frame.linesize[2], chromaWidth, 200);
    }
}

/// @brief 使用当前预编译 FFmpeg 在测试输出目录生成极小 AVI/MJPEG。
/// @param filePath 输出文件路径。
/// @return 完整写入容器时返回 true。
bool createTestVideo(const std::filesystem::path& filePath)
{
    AVFormatContext*  rawFormatContext{ nullptr };
    const std::string path = filePath.string();
    if ( avformat_alloc_output_context2(
             &rawFormatContext, nullptr, "avi", path.c_str()) < 0 ||
         !rawFormatContext ) {
        return false;
    }
    OutputContextPtr formatContext(rawFormatContext);

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if ( !encoder ) return false;

    AVStream* stream = avformat_new_stream(formatContext.get(), nullptr);
    if ( !stream ) return false;

    CodecContextPtr codecContext(avcodec_alloc_context3(encoder));
    if ( !codecContext ) return false;

    codecContext->codec_id    = AV_CODEC_ID_MJPEG;
    codecContext->codec_type  = AVMEDIA_TYPE_VIDEO;
    codecContext->width       = TEST_VIDEO_WIDTH;
    codecContext->height      = TEST_VIDEO_HEIGHT;
    codecContext->pix_fmt     = AV_PIX_FMT_YUVJ420P;
    codecContext->time_base   = AVRational{ 1, TEST_VIDEO_FPS };
    codecContext->framerate   = AVRational{ TEST_VIDEO_FPS, 1 };
    codecContext->color_range = AVCOL_RANGE_JPEG;
    if ( formatContext->oformat->flags & AVFMT_GLOBALHEADER ) {
        codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if ( avcodec_open2(codecContext.get(), encoder, nullptr) < 0 ) return false;

    stream->time_base = codecContext->time_base;
    if ( avcodec_parameters_from_context(stream->codecpar, codecContext.get()) <
         0 ) {
        return false;
    }

    if ( !(formatContext->oformat->flags & AVFMT_NOFILE) &&
         avio_open(&formatContext->pb, path.c_str(), AVIO_FLAG_WRITE) < 0 ) {
        return false;
    }
    if ( avformat_write_header(formatContext.get(), nullptr) < 0 ) return false;

    FramePtr  frame(av_frame_alloc());
    PacketPtr packet(av_packet_alloc());
    if ( !frame || !packet ) return false;

    frame->format = codecContext->pix_fmt;
    frame->width  = codecContext->width;
    frame->height = codecContext->height;
    if ( av_frame_get_buffer(frame.get(), 32) < 0 ) return false;

    for ( int frameIndex = 0; frameIndex < TEST_VIDEO_FRAME_COUNT;
          ++frameIndex ) {
        if ( av_frame_make_writable(frame.get()) < 0 ) return false;
        fillTestFrame(*frame, frameIndex);
        frame->pts = frameIndex;
        if ( avcodec_send_frame(codecContext.get(), frame.get()) < 0 ||
             !writeAvailablePackets(
                 *codecContext, *formatContext, *stream, *packet) ) {
            return false;
        }
    }

    if ( avcodec_send_frame(codecContext.get(), nullptr) < 0 ||
         !writeAvailablePackets(
             *codecContext, *formatContext, *stream, *packet) ) {
        return false;
    }

    return av_write_trailer(formatContext.get()) >= 0;
}

/// @brief 生成带 BT.709 limited-range 标记的 Matroska/MPEG-4 视频。
/// @param filePath 输出文件路径。
/// @return 完整写入容器时返回 true。
bool createBt709ColorVideo(const std::filesystem::path& filePath)
{
    AVFormatContext*  rawFormatContext{ nullptr };
    const std::string path = filePath.string();
    if ( avformat_alloc_output_context2(
             &rawFormatContext, nullptr, "matroska", path.c_str()) < 0 ||
         !rawFormatContext ) {
        XERROR("VideoFrameDecoderTest: Matroska muxer is unavailable");
        return false;
    }
    OutputContextPtr formatContext(rawFormatContext);

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if ( !encoder ) {
        XERROR("VideoFrameDecoderTest: MPEG-4 encoder is unavailable");
        return false;
    }

    AVStream* stream = avformat_new_stream(formatContext.get(), nullptr);
    if ( !stream ) return false;

    CodecContextPtr codecContext(avcodec_alloc_context3(encoder));
    if ( !codecContext ) return false;

    codecContext->codec_id        = AV_CODEC_ID_MPEG4;
    codecContext->codec_type      = AVMEDIA_TYPE_VIDEO;
    codecContext->width           = TEST_VIDEO_WIDTH;
    codecContext->height          = TEST_VIDEO_HEIGHT;
    codecContext->pix_fmt         = AV_PIX_FMT_YUV420P;
    codecContext->time_base       = AVRational{ 1, 1 };
    codecContext->framerate       = AVRational{ 1, 1 };
    codecContext->bit_rate        = 1'000'000;
    codecContext->gop_size        = 1;
    codecContext->max_b_frames    = 0;
    codecContext->color_range     = AVCOL_RANGE_MPEG;
    codecContext->colorspace      = AVCOL_SPC_BT709;
    codecContext->color_primaries = AVCOL_PRI_BT709;
    codecContext->color_trc       = AVCOL_TRC_BT709;
    if ( formatContext->oformat->flags & AVFMT_GLOBALHEADER ) {
        codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if ( avcodec_open2(codecContext.get(), encoder, nullptr) < 0 ) return false;

    stream->time_base = codecContext->time_base;
    if ( avcodec_parameters_from_context(stream->codecpar, codecContext.get()) <
         0 ) {
        return false;
    }

    if ( !(formatContext->oformat->flags & AVFMT_NOFILE) &&
         avio_open(&formatContext->pb, path.c_str(), AVIO_FLAG_WRITE) < 0 ) {
        return false;
    }
    if ( avformat_write_header(formatContext.get(), nullptr) < 0 ) return false;

    FramePtr  frame(av_frame_alloc());
    PacketPtr packet(av_packet_alloc());
    if ( !frame || !packet ) return false;

    frame->format          = codecContext->pix_fmt;
    frame->width           = codecContext->width;
    frame->height          = codecContext->height;
    frame->color_range     = codecContext->color_range;
    frame->colorspace      = codecContext->colorspace;
    frame->color_primaries = codecContext->color_primaries;
    frame->color_trc       = codecContext->color_trc;
    if ( av_frame_get_buffer(frame.get(), 32) < 0 ||
         av_frame_make_writable(frame.get()) < 0 ) {
        return false;
    }

    fillBt709ColorFrame(*frame);
    frame->pts = 0;
    if ( avcodec_send_frame(codecContext.get(), frame.get()) < 0 ||
         !writeAvailablePackets(
             *codecContext, *formatContext, *stream, *packet) ||
         avcodec_send_frame(codecContext.get(), nullptr) < 0 ||
         !writeAvailablePackets(
             *codecContext, *formatContext, *stream, *packet) ) {
        return false;
    }

    return av_write_trailer(formatContext.get()) >= 0;
}

/// @brief 验证探测、顺序解码、大跨度 Seek 与倒退 Seek。
/// @param outputDirectory 测试生成文件目录。
/// @return 所有行为符合预期时返回 true。
bool testVideoDecode(const std::filesystem::path& outputDirectory)
{
    std::error_code filesystemError;
    std::filesystem::create_directories(outputDirectory, filesystemError);
    if ( filesystemError ) return false;

    const std::filesystem::path videoPath = outputDirectory / "tiny_mjpeg.avi";
    std::filesystem::remove(videoPath, filesystemError);
    filesystemError.clear();
    if ( !createTestVideo(videoPath) ) return false;

    const auto probed = MMM::Utils::probeVideoInfo(videoPath);
    if ( !probed || probed->width != TEST_VIDEO_WIDTH ||
         probed->height != TEST_VIDEO_HEIGHT || probed->duration <= 0.0 ) {
        return false;
    }

    MMM::Utils::VideoFrameDecoder decoder;
    if ( !decoder.open(videoPath) || !decoder.isOpen() ||
         decoder.info().width != TEST_VIDEO_WIDTH ||
         decoder.info().height != TEST_VIDEO_HEIGHT ) {
        return false;
    }

    MMM::Utils::VideoFrame firstFrame;
    MMM::Utils::VideoFrame middleFrame;
    MMM::Utils::VideoFrame jumpedFrame;
    MMM::Utils::VideoFrame rewoundFrame;
    MMM::Utils::VideoFrame endFrame;
    const auto*            decodedFrame = decoder.decodeFrameAt(0.0);
    if ( !decodedFrame ) return false;
    const std::uint8_t* firstFramePixels = decodedFrame->rgba.data();
    firstFrame                           = *decodedFrame;
    decodedFrame                         = decoder.decodeFrameAt(0.1);
    if ( !decodedFrame || decodedFrame->timestamp != firstFrame.timestamp ||
         decodedFrame->rgba.data() != firstFramePixels ) {
        return false;
    }
    decodedFrame = decoder.decodeFrameAt(0.7);
    if ( !decodedFrame ) return false;
    middleFrame  = *decodedFrame;
    decodedFrame = decoder.decodeFrameAt(2.1);
    if ( !decodedFrame ) return false;
    jumpedFrame  = *decodedFrame;
    decodedFrame = decoder.decodeFrameAt(0.0);
    if ( !decodedFrame ) return false;
    rewoundFrame = *decodedFrame;
    decodedFrame = decoder.decodeFrameAt(probed->duration + 10.0);
    if ( !decodedFrame ) return false;
    endFrame = *decodedFrame;

    const std::size_t expectedBytes =
        static_cast<std::size_t>(TEST_VIDEO_WIDTH) * TEST_VIDEO_HEIGHT * 4U;
    if ( firstFrame.rgba.size() != expectedBytes ||
         middleFrame.rgba.size() != expectedBytes ||
         jumpedFrame.rgba.size() != expectedBytes ||
         rewoundFrame.rgba.size() != expectedBytes ||
         endFrame.rgba.size() != expectedBytes ) {
        return false;
    }
    if ( firstFrame.timestamp > 0.01 || middleFrame.timestamp > 0.7 ||
         jumpedFrame.timestamp > 2.1 || rewoundFrame.timestamp > 0.01 ||
         endFrame.timestamp > probed->duration ||
         endFrame.timestamp < jumpedFrame.timestamp ) {
        return false;
    }
    if ( jumpedFrame.rgba[0] <= firstFrame.rgba[0] ||
         rewoundFrame.rgba[0] != firstFrame.rgba[0] ) {
        return false;
    }

    const double nan = std::numeric_limits<double>::quiet_NaN();
    if ( decoder.decodeFrameAt(nan) ) return false;

    decoder.close();
    return !decoder.isOpen() && decoder.info().width == 0 &&
           decoder.info().height == 0;
}

/// @brief 验证不存在的路径不会返回伪造视频信息。
/// @param outputDirectory 测试输出目录。
/// @return 无效路径被拒绝时返回 true。
bool testInvalidPath(const std::filesystem::path& outputDirectory)
{
    return !MMM::Utils::probeVideoInfo(outputDirectory / "missing-video.avi");
}

/// @brief 验证帧级色彩属性会驱动 BT.709 到 RGBA 的转换矩阵。
/// @param outputDirectory 测试输出目录。
/// @return 输出像素符合 BT.709 limited-range 转换时返回 true。
bool testBt709ColorConversion(const std::filesystem::path& outputDirectory)
{
    const std::filesystem::path videoPath =
        outputDirectory / "bt709_color_mpeg4.mkv";
    std::error_code filesystemError;
    std::filesystem::remove(videoPath, filesystemError);
    if ( !createBt709ColorVideo(videoPath) ) {
        XERROR("VideoFrameDecoderTest: Failed to create BT.709 fixture");
        return false;
    }

    MMM::Utils::VideoFrameDecoder decoder;
    if ( !decoder.open(videoPath) ) return false;
    const MMM::Utils::VideoFrame* frame = decoder.decodeFrameAt(0.0);
    if ( !frame || frame->rgba.size() < 4 ) return false;

    const auto channelNear = [](std::uint8_t value, int expected) {
        return std::abs(static_cast<int>(value) - expected) <= 5;
    };
    const bool convertedWithBt709 =
        channelNear(frame->rgba[0], 226) && channelNear(frame->rgba[1], 42) &&
        channelNear(frame->rgba[2], 249) && frame->rgba[3] == 255;
    if ( !convertedWithBt709 ) {
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
bool testExternalProbeFile()
{
    const char* probePath = std::getenv("MMM_VIDEO_PROBE_FILE");
    if ( !probePath || probePath[0] == '\0' ) {
        return true;
    }

    const std::filesystem::path videoPath(probePath);
    const auto                  info = MMM::Utils::probeVideoInfo(videoPath);
    if ( !info || info->width == 0 || info->height == 0 ) {
        XERROR("VideoFrameDecoderTest: Failed to probe external video {}",
               videoPath.string());
        return false;
    }

    MMM::Utils::VideoFrameDecoder decoder;
    const double                  targetTime =
        info->duration > 0.0 ? std::min(1.0, info->duration * 0.5) : 0.0;
    if ( !decoder.open(videoPath) ) {
        XERROR("VideoFrameDecoderTest: Failed to open external video {}",
               videoPath.string());
        return false;
    }
    const MMM::Utils::VideoFrame* frame = decoder.decodeFrameAt(targetTime);
    if ( !frame || frame->rgba.empty() || frame->width != info->width ||
         frame->height != info->height ) {
        XERROR("VideoFrameDecoderTest: Failed to decode external video {}",
               videoPath.string());
        return false;
    }

    /// @brief 模拟暂停状态下连续拖动进度条产生的前后跳转序列。
    constexpr std::array<double, 12> scrubFractions = {
        0.83, 0.12, 0.68, 0.24, 0.91, 0.37, 0.55, 0.08, 0.76, 0.43, 0.97, 0.31,
    };
    if ( info->duration > 0.0 ) {
        for ( const double fraction : scrubFractions ) {
            const double scrubTime = info->duration * fraction;
            frame                  = decoder.decodeFrameAt(scrubTime);
            if ( !frame || frame->rgba.empty() || frame->width != info->width ||
                 frame->height != info->height ) {
                XERROR(
                    "VideoFrameDecoderTest: Scrub decode failed for {} at "
                    "{:.3f}s",
                    videoPath.string(),
                    scrubTime);
                return false;
            }
        }
    }

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
    if ( argc < 2 ) {
        XERROR("VideoFrameDecoderTest: Missing output directory");
        return 1;
    }

    const std::filesystem::path outputDirectory(argv[1]);
    return testVideoDecode(outputDirectory) &&
                   testInvalidPath(outputDirectory) &&
                   testBt709ColorConversion(outputDirectory) &&
                   testExternalProbeFile()
               ? 0
               : 1;
}
