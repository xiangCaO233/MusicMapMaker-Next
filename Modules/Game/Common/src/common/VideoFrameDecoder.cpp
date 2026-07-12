#include "common/VideoFrameDecoder.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace MMM::Utils
{
namespace
{

/// @brief 超过该跨度的正向时间请求通过 Seek 避免逐帧扫描。
constexpr double LARGE_FORWARD_SEEK_SECONDS = 1.0;

/// @brief 比较浮点时间戳时允许的微小误差。
constexpr double TIMESTAMP_EPSILON_SECONDS = 1e-6;

/// @brief 释放 FFmpeg 输入容器上下文。
struct FormatContextDeleter {
    /// @brief 关闭输入并释放容器上下文。
    void operator()(AVFormatContext* context) const
    {
        if ( context ) avformat_close_input(&context);
    }
};

/// @brief 释放 FFmpeg 编解码器上下文。
struct CodecContextDeleter {
    /// @brief 释放解码器上下文并清空临时指针。
    void operator()(AVCodecContext* context) const
    {
        if ( context ) avcodec_free_context(&context);
    }
};

/// @brief 释放 FFmpeg 数据包。
struct PacketDeleter {
    /// @brief 释放数据包并清空临时指针。
    void operator()(AVPacket* packet) const
    {
        if ( packet ) av_packet_free(&packet);
    }
};

/// @brief 释放 FFmpeg 视频帧。
struct FrameDeleter {
    /// @brief 释放视频帧并清空临时指针。
    void operator()(AVFrame* frame) const
    {
        if ( frame ) av_frame_free(&frame);
    }
};

/// @brief 释放 libswscale 上下文。
struct ScaleContextDeleter {
    /// @brief 使用当前预编译 FFmpeg 的二级指针接口释放转换器。
    void operator()(SwsContext* context) const
    {
        if ( context ) sws_free_context(&context);
    }
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr  = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr        = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr         = std::unique_ptr<AVFrame, FrameDeleter>;
using ScaleContextPtr  = std::unique_ptr<SwsContext, ScaleContextDeleter>;

/// @brief 将 FFmpeg 错误码转换为日志可读文本。
/// @param errorCode FFmpeg 返回的负错误码。
/// @return 对应错误描述。
std::string ffmpegErrorText(int errorCode)
{
    char errorBuffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(errorCode, errorBuffer, sizeof(errorBuffer));
    return std::string(errorBuffer);
}

/// @brief 从流级或容器级时长中选择可用视频时长。
/// @param formatContext 已读取流信息的容器。
/// @param stream 目标视频流。
/// @return 非负秒数，未知时为零。
double resolveDurationSeconds(const AVFormatContext& formatContext,
                              const AVStream&        stream)
{
    if ( stream.duration != AV_NOPTS_VALUE && stream.duration > 0 ) {
        const double duration =
            static_cast<double>(stream.duration) * av_q2d(stream.time_base);
        if ( std::isfinite(duration) && duration > 0.0 ) return duration;
    }

    if ( formatContext.duration != AV_NOPTS_VALUE &&
         formatContext.duration > 0 ) {
        const double duration = static_cast<double>(formatContext.duration) /
                                static_cast<double>(AV_TIME_BASE);
        if ( std::isfinite(duration) && duration > 0.0 ) return duration;
    }
    return 0.0;
}

}  // namespace

/// @brief 隐藏 FFmpeg 资源和按时间选择帧所需缓存。
struct VideoFrameDecoder::Impl {
    /// @brief 打开视频并以事务方式接管所有成功初始化的资源。
    /// @param filePath 视频文件路径。
    /// @return 初始化成功时返回 true。
    bool open(const std::filesystem::path& filePath)
    {
        close();

        std::error_code filesystemError;
        if ( !std::filesystem::is_regular_file(filePath, filesystemError) ||
             filesystemError ) {
            XERROR("VideoFrameDecoder: Video file not found: {}",
                   Config::pathToUtf8(filePath));
            return false;
        }

        const std::string pathUtf8 = Config::pathToUtf8(filePath);
        AVFormatContext*  rawFormatContext{ nullptr };
        int               result = avformat_open_input(
            &rawFormatContext, pathUtf8.c_str(), nullptr, nullptr);
        FormatContextPtr openedFormat(rawFormatContext);
        if ( result < 0 || !openedFormat ) {
            XERROR("VideoFrameDecoder: Failed to open {}: {}",
                   pathUtf8,
                   ffmpegErrorText(result));
            return false;
        }

        result = avformat_find_stream_info(openedFormat.get(), nullptr);
        if ( result < 0 ) {
            XERROR("VideoFrameDecoder: Failed to read streams from {}: {}",
                   pathUtf8,
                   ffmpegErrorText(result));
            return false;
        }

        const AVCodec* decoder{ nullptr };
        const int      openedStreamIndex = av_find_best_stream(
            openedFormat.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if ( openedStreamIndex < 0 || !decoder ) {
            XERROR("VideoFrameDecoder: No decodable video stream in {}: {}",
                   pathUtf8,
                   ffmpegErrorText(openedStreamIndex));
            return false;
        }

        AVStream* openedStream =
            openedFormat->streams[static_cast<unsigned int>(openedStreamIndex)];
        if ( !openedStream || !openedStream->codecpar ) {
            XERROR("VideoFrameDecoder: Invalid video stream in {}", pathUtf8);
            return false;
        }

        CodecContextPtr openedCodec(avcodec_alloc_context3(decoder));
        if ( !openedCodec ) {
            XERROR("VideoFrameDecoder: Failed to allocate decoder for {}",
                   pathUtf8);
            return false;
        }

        result = avcodec_parameters_to_context(openedCodec.get(),
                                               openedStream->codecpar);
        if ( result < 0 ) {
            XERROR("VideoFrameDecoder: Failed to configure decoder for {}: {}",
                   pathUtf8,
                   ffmpegErrorText(result));
            return false;
        }

        result = avcodec_open2(openedCodec.get(), decoder, nullptr);
        if ( result < 0 ) {
            XERROR("VideoFrameDecoder: Failed to initialize decoder for {}: {}",
                   pathUtf8,
                   ffmpegErrorText(result));
            return false;
        }

        if ( openedCodec->width <= 0 || openedCodec->height <= 0 ) {
            XERROR("VideoFrameDecoder: Invalid video dimensions in {}: {}x{}",
                   pathUtf8,
                   openedCodec->width,
                   openedCodec->height);
            return false;
        }

        PacketPtr openedPacket(av_packet_alloc());
        FramePtr  openedFrame(av_frame_alloc());
        FramePtr  openedConvertedFrame(av_frame_alloc());
        if ( !openedPacket || !openedFrame || !openedConvertedFrame ) {
            XERROR(
                "VideoFrameDecoder: Failed to allocate decode buffers for {}",
                pathUtf8);
            return false;
        }

        VideoInfo openedInfo;
        openedInfo.width  = static_cast<std::uint32_t>(openedCodec->width);
        openedInfo.height = static_cast<std::uint32_t>(openedCodec->height);
        openedInfo.duration =
            resolveDurationSeconds(*openedFormat, *openedStream);

        formatContext  = std::move(openedFormat);
        codecContext   = std::move(openedCodec);
        packet         = std::move(openedPacket);
        decodedFrame   = std::move(openedFrame);
        convertedFrame = std::move(openedConvertedFrame);
        streamIndex    = openedStreamIndex;
        videoInfo      = openedInfo;
        mediaPath      = pathUtf8;
        resetDecodeState();

        XINFO("VideoFrameDecoder: Opened {} [{}x{}, {:.3f}s]",
              pathUtf8,
              videoInfo.width,
              videoInfo.height,
              videoInfo.duration);
        return true;
    }

    /// @brief 释放当前视频与全部解码缓存。
    void close()
    {
        scaleContext.reset();
        convertedFrame.reset();
        decodedFrame.reset();
        packet.reset();
        codecContext.reset();
        formatContext.reset();
        streamIndex = -1;
        videoInfo   = {};
        mediaPath.clear();
        resetDecodeState();
    }

    /// @brief 判断核心容器、解码器和视频流是否有效。
    /// @return 资源完整时返回 true。
    bool isOpen() const
    {
        return formatContext && codecContext && packet && decodedFrame &&
               convertedFrame && streamIndex >= 0;
    }

    /// @brief 选取目标时间点最近且不晚于目标的帧。
    /// @param seconds 相对视频起点时间。
    /// @return 解码器内部当前帧观察指针；失败时返回 nullptr。
    const VideoFrame* decodeFrameAt(double seconds)
    {
        if ( !isOpen() || !std::isfinite(seconds) ) return nullptr;

        double targetSeconds = std::max(0.0, seconds);
        if ( videoInfo.duration > 0.0 && targetSeconds >= videoInfo.duration ) {
            targetSeconds =
                std::max(0.0, videoInfo.duration - TIMESTAMP_EPSILON_SECONDS);
        }
        const bool movesBackward =
            hasLastRequest &&
            targetSeconds + TIMESTAMP_EPSILON_SECONDS < lastRequestSeconds;
        const bool makesLargeForwardJump =
            (!hasLastRequest && targetSeconds > LARGE_FORWARD_SEEK_SECONDS) ||
            (hasLastRequest &&
             targetSeconds - lastRequestSeconds > LARGE_FORWARD_SEEK_SECONDS);

        if ( (movesBackward || makesLargeForwardJump) &&
             !seekTo(targetSeconds) ) {
            return nullptr;
        }

        hasLastRequest     = true;
        lastRequestSeconds = targetSeconds;

        while ( true ) {
            if ( hasPendingFrame ) {
                if ( pendingFrame.timestamp <=
                     targetSeconds + TIMESTAMP_EPSILON_SECONDS ) {
                    currentFrame    = std::move(pendingFrame);
                    hasCurrentFrame = true;
                    hasPendingFrame = false;
                    continue;
                }
                break;
            }

            VideoFrame nextFrame;
            if ( !decodeNextFrame(nextFrame) ) break;

            if ( nextFrame.timestamp <=
                 targetSeconds + TIMESTAMP_EPSILON_SECONDS ) {
                currentFrame    = std::move(nextFrame);
                hasCurrentFrame = true;
                continue;
            }

            pendingFrame    = std::move(nextFrame);
            hasPendingFrame = true;
            break;
        }

        return hasCurrentFrame ? &currentFrame : nullptr;
    }

    /// @brief 重置 packet、帧选择和 EOF 状态但保留已打开资源。
    void resetDecodeState()
    {
        sentDrainPacket         = false;
        reachedEndOfStream      = false;
        hasCurrentFrame         = false;
        hasPendingFrame         = false;
        hasLastRequest          = false;
        hasLastDecodedTimestamp = false;
        lastRequestSeconds      = 0.0;
        lastDecodedTimestamp    = 0.0;
        currentFrame            = {};
        pendingFrame            = {};
    }

    /// @brief Seek 到目标时间之前的关键帧并清空解码器内部状态。
    /// @param seconds 相对视频起点时间。
    /// @return Seek 成功时返回 true。
    bool seekTo(double seconds)
    {
        AVStream* stream = videoStream();
        if ( !stream ) return false;
        if ( seconds >
             static_cast<double>(std::numeric_limits<std::int64_t>::max()) /
                 static_cast<double>(AV_TIME_BASE) ) {
            return false;
        }

        const std::int64_t relativeTimestamp =
            av_rescale_q(static_cast<std::int64_t>(std::llround(
                             seconds * static_cast<double>(AV_TIME_BASE))),
                         AV_TIME_BASE_Q,
                         stream->time_base);
        const std::int64_t streamStart =
            stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
        const int result = av_seek_frame(formatContext.get(),
                                         streamIndex,
                                         streamStart + relativeTimestamp,
                                         AVSEEK_FLAG_BACKWARD);
        if ( result < 0 ) {
            XERROR("VideoFrameDecoder: Failed to seek {} to {:.3f}s: {}",
                   mediaPath,
                   seconds,
                   ffmpegErrorText(result));
            return false;
        }

        avcodec_flush_buffers(codecContext.get());
        av_packet_unref(packet.get());
        av_frame_unref(decodedFrame.get());
        resetDecodeState();
        lastDecodedTimestamp = seconds;
        return true;
    }

    /// @brief 解码输出序列中的下一帧并转换为 RGBA8。
    /// @param output 接收转换结果。
    /// @return 成功取得一帧时返回 true。
    bool decodeNextFrame(VideoFrame& output)
    {
        if ( reachedEndOfStream ) return false;

        while ( true ) {
            const int receiveResult =
                avcodec_receive_frame(codecContext.get(), decodedFrame.get());
            if ( receiveResult == 0 ) {
                const bool converted = convertFrame(*decodedFrame, output);
                av_frame_unref(decodedFrame.get());
                return converted;
            }
            if ( receiveResult == AVERROR_EOF ) {
                reachedEndOfStream = true;
                return false;
            }
            if ( receiveResult != AVERROR(EAGAIN) ) {
                XERROR("VideoFrameDecoder: Failed to receive frame from {}: {}",
                       mediaPath,
                       ffmpegErrorText(receiveResult));
                reachedEndOfStream = true;
                return false;
            }

            if ( sentDrainPacket ) {
                reachedEndOfStream = true;
                return false;
            }

            int readResult{ 0 };
            do {
                av_packet_unref(packet.get());
                readResult = av_read_frame(formatContext.get(), packet.get());
            } while ( readResult >= 0 && packet->stream_index != streamIndex );

            if ( readResult == AVERROR_EOF ) {
                const int drainResult =
                    avcodec_send_packet(codecContext.get(), nullptr);
                if ( drainResult < 0 && drainResult != AVERROR_EOF ) {
                    XERROR("VideoFrameDecoder: Failed to drain {}: {}",
                           mediaPath,
                           ffmpegErrorText(drainResult));
                    reachedEndOfStream = true;
                    return false;
                }
                sentDrainPacket = true;
                continue;
            }
            if ( readResult < 0 ) {
                XERROR("VideoFrameDecoder: Failed to read {}: {}",
                       mediaPath,
                       ffmpegErrorText(readResult));
                reachedEndOfStream = true;
                return false;
            }

            const int sendResult =
                avcodec_send_packet(codecContext.get(), packet.get());
            av_packet_unref(packet.get());
            if ( sendResult < 0 ) {
                XWARN(
                    "VideoFrameDecoder: Skipping undecodable packet in {}: {}",
                    mediaPath,
                    ffmpegErrorText(sendResult));
            }
        }
    }

    /// @brief 将 FFmpeg 原生帧转换为紧密排列 RGBA8。
    /// @param sourceFrame 解码器输出帧。
    /// @param output 转换后的帧。
    /// @return 像素转换成功时返回 true。
    bool convertFrame(const AVFrame& sourceFrame, VideoFrame& output)
    {
        const int width  = sourceFrame.width;
        const int height = sourceFrame.height;
        if ( width <= 0 || height <= 0 ||
             sourceFrame.format == AV_PIX_FMT_NONE ) {
            return false;
        }

        const std::size_t unsignedWidth  = static_cast<std::size_t>(width);
        const std::size_t unsignedHeight = static_cast<std::size_t>(height);
        if ( unsignedWidth >
             std::numeric_limits<std::size_t>::max() / unsignedHeight / 4U ) {
            return false;
        }

        if ( !scaleContext ) {
            ScaleContextPtr allocatedContext(sws_alloc_context());
            if ( allocatedContext ) {
                allocatedContext->flags = SWS_BILINEAR;
            }
            scaleContext = std::move(allocatedContext);
        }
        if ( !scaleContext ) {
            XERROR("VideoFrameDecoder: Failed to create RGBA converter for {}",
                   mediaPath);
            return false;
        }

        output.rgba.resize(unsignedWidth * unsignedHeight * 4U);
        av_frame_unref(convertedFrame.get());
        convertedFrame->format              = AV_PIX_FMT_RGBA;
        convertedFrame->width               = width;
        convertedFrame->height              = height;
        convertedFrame->colorspace          = AVCOL_SPC_RGB;
        convertedFrame->color_range         = AVCOL_RANGE_JPEG;
        convertedFrame->color_primaries     = sourceFrame.color_primaries;
        convertedFrame->color_trc           = sourceFrame.color_trc;
        convertedFrame->sample_aspect_ratio = sourceFrame.sample_aspect_ratio;
        convertedFrame->data[0]             = output.rgba.data();
        convertedFrame->linesize[0]         = width * 4;
        convertedFrame->extended_data       = convertedFrame->data;

        const int scaleResult = sws_scale_frame(
            scaleContext.get(), convertedFrame.get(), &sourceFrame);
        av_frame_unref(convertedFrame.get());
        if ( scaleResult < 0 ) {
            XERROR("VideoFrameDecoder: Failed to convert frame from {}: {}",
                   mediaPath,
                   ffmpegErrorText(scaleResult));
            output = {};
            return false;
        }

        output.width     = static_cast<std::uint32_t>(width);
        output.height    = static_cast<std::uint32_t>(height);
        output.timestamp = resolveFrameTimestamp(sourceFrame);
        return true;
    }

    /// @brief 依据 best_effort_timestamp 解析 VFR 帧显示时间。
    /// @param sourceFrame 解码器输出帧。
    /// @return 相对视频起点的非负秒数。
    double resolveFrameTimestamp(const AVFrame& sourceFrame)
    {
        AVStream* stream = videoStream();
        if ( !stream ) return 0.0;

        std::int64_t timestamp = sourceFrame.best_effort_timestamp;
        if ( timestamp == AV_NOPTS_VALUE ) timestamp = sourceFrame.pts;

        double seconds = lastDecodedTimestamp;
        if ( timestamp != AV_NOPTS_VALUE ) {
            const std::int64_t streamStart =
                stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
            seconds = static_cast<double>(timestamp - streamStart) *
                      av_q2d(stream->time_base);
        } else if ( hasLastDecodedTimestamp ) {
            const AVRational guessedRate =
                av_guess_frame_rate(formatContext.get(), stream, nullptr);
            if ( guessedRate.num > 0 && guessedRate.den > 0 ) {
                seconds += av_q2d(av_inv_q(guessedRate));
            }
        }

        if ( !std::isfinite(seconds) ) seconds = 0.0;
        seconds                 = std::max(0.0, seconds);
        lastDecodedTimestamp    = seconds;
        hasLastDecodedTimestamp = true;
        return seconds;
    }

    /// @brief 获取当前视频流观察指针。
    /// @return 流索引有效时返回视频流，否则返回空指针。
    AVStream* videoStream() const
    {
        if ( !formatContext || streamIndex < 0 ||
             static_cast<unsigned int>(streamIndex) >=
                 formatContext->nb_streams ) {
            return nullptr;
        }
        return formatContext->streams[static_cast<unsigned int>(streamIndex)];
    }

    /// @brief 输入容器上下文。
    FormatContextPtr formatContext;

    /// @brief 视频解码器上下文。
    CodecContextPtr codecContext;

    /// @brief 复用的数据包。
    PacketPtr packet;

    /// @brief 复用的原生解码帧。
    FramePtr decodedFrame;

    /// @brief 复用的 RGBA 目标帧描述；像素内存由 VideoFrame::rgba 持有。
    FramePtr convertedFrame;

    /// @brief 复用的 RGBA 像素转换器。
    ScaleContextPtr scaleContext;

    /// @brief 当前视频流索引。
    int streamIndex{ -1 };

    /// @brief 当前视频基础信息。
    VideoInfo videoInfo;

    /// @brief 当前媒体 UTF-8 路径，仅用于诊断日志。
    std::string mediaPath;

    /// @brief 当前目标时间点可显示的最近帧。
    VideoFrame currentFrame;

    /// @brief 已提前解出但晚于当前目标时间的下一帧。
    VideoFrame pendingFrame;

    /// @brief 是否持有当前可显示帧。
    bool hasCurrentFrame{ false };

    /// @brief 是否持有提前解出的下一帧。
    bool hasPendingFrame{ false };

    /// @brief 是否已经向解码器发送 EOF drain 包。
    bool sentDrainPacket{ false };

    /// @brief 是否已完全消费视频流。
    bool reachedEndOfStream{ false };

    /// @brief 是否已有上一次时间点请求。
    bool hasLastRequest{ false };

    /// @brief 上一次请求时间，供判断倒退与大跨度跳转。
    double lastRequestSeconds{ 0.0 };

    /// @brief 是否已有可靠或回退生成的解码帧时间戳。
    bool hasLastDecodedTimestamp{ false };

    /// @brief 最近一次解码帧相对视频起点的时间。
    double lastDecodedTimestamp{ 0.0 };
};

VideoFrameDecoder::VideoFrameDecoder() : m_impl(std::make_unique<Impl>()) {}

VideoFrameDecoder::~VideoFrameDecoder() = default;

bool VideoFrameDecoder::open(const std::filesystem::path& filePath)
{
    return m_impl->open(filePath);
}

void VideoFrameDecoder::close()
{
    m_impl->close();
}

bool VideoFrameDecoder::isOpen() const
{
    return m_impl->isOpen();
}

const VideoInfo& VideoFrameDecoder::info() const
{
    return m_impl->videoInfo;
}

const VideoFrame* VideoFrameDecoder::decodeFrameAt(double seconds)
{
    return m_impl->decodeFrameAt(seconds);
}

std::optional<VideoInfo> probeVideoInfo(const std::filesystem::path& filePath)
{
    VideoFrameDecoder decoder;
    if ( !decoder.open(filePath) ) return std::nullopt;
    return decoder.info();
}

}  // namespace MMM::Utils
