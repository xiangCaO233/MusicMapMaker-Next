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
/// @note 一秒阈值平衡短距离顺序解码与关键帧跳转开销。
constexpr double LARGE_FORWARD_SEEK_SECONDS = 1.0;

/// @brief 比较浮点时间戳时允许的微小误差。
/// @note 误差只吸收时间基换算舍入，不用于扩大帧选择窗口。
constexpr double TIMESTAMP_EPSILON_SECONDS = 1e-6;

/// @brief 释放 FFmpeg 输入容器上下文。
struct FormatContextDeleter {
    /// @brief 关闭输入并释放容器上下文。
    /// @param context 由 avformat_open_input 创建的上下文；允许为空。
    void operator()(AVFormatContext* context) const
    {
        // FFmpeg 接口接收二级指针并在释放后写空临时参数。
        if ( context ) avformat_close_input(&context);
    }
};

/// @brief 释放 FFmpeg 编解码器上下文。
struct CodecContextDeleter {
    /// @brief 释放解码器上下文并清空临时指针。
    /// @param context 由 avcodec_alloc_context3 分配的上下文；允许为空。
    void operator()(AVCodecContext* context) const
    {
        // 自定义删除器将 C API 资源纳入 unique_ptr 的异常安全生命周期。
        if ( context ) avcodec_free_context(&context);
    }
};

/// @brief 释放 FFmpeg 数据包。
struct PacketDeleter {
    /// @brief 释放数据包并清空临时指针。
    /// @param packet 由 av_packet_alloc 分配的数据包；允许为空。
    void operator()(AVPacket* packet) const
    {
        // free 同时释放包结构及仍被引用的底层缓冲。
        if ( packet ) av_packet_free(&packet);
    }
};

/// @brief 释放 FFmpeg 视频帧。
struct FrameDeleter {
    /// @brief 释放视频帧并清空临时指针。
    /// @param frame 由 av_frame_alloc 分配的帧；允许为空。
    void operator()(AVFrame* frame) const
    {
        // 帧删除器适用于解码源帧和不拥有像素的转换目标帧描述。
        if ( frame ) av_frame_free(&frame);
    }
};

/// @brief 释放 libswscale 上下文。
struct ScaleContextDeleter {
    /// @brief 使用当前预编译 FFmpeg 的二级指针接口释放转换器。
    /// @param context 由 sws_alloc_context 创建的像素转换上下文；允许为空。
    void operator()(SwsContext* context) const
    {
        // 当前 FFmpeg 版本的释放接口会清空传入的临时指针。
        if ( context ) sws_free_context(&context);
    }
};

/// @brief 输入容器上下文的独占 RAII 句柄。
using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
/// @brief 编解码器上下文的独占 RAII 句柄。
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
/// @brief 可复用数据包的独占 RAII 句柄。
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
/// @brief 解码帧或转换帧描述的独占 RAII 句柄。
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
/// @brief libswscale 转换状态的独占 RAII 句柄。
using ScaleContextPtr = std::unique_ptr<SwsContext, ScaleContextDeleter>;

/// @brief 将 FFmpeg 错误码转换为日志可读文本。
/// @param errorCode FFmpeg 返回的负错误码。
/// @return 对应错误描述。
std::string ffmpegErrorText(int errorCode)
{
    // 固定栈缓冲遵循 FFmpeg 要求的最大错误文本容量。
    char errorBuffer[AV_ERROR_MAX_STRING_SIZE]{};
    // av_strerror 失败时缓冲仍保持零初始化，可安全构造空字符串。
    av_strerror(errorCode, errorBuffer, sizeof(errorBuffer));
    // 返回自有字符串，日志格式化不依赖栈缓冲生命周期。
    return std::string(errorBuffer);
}

/// @brief 从流级或容器级时长中选择可用视频时长。
/// @param formatContext 已读取流信息的容器。
/// @param stream 目标视频流。
/// @return 非负秒数，未知时为零。
double resolveDurationSeconds(const AVFormatContext& formatContext,
                              const AVStream&        stream)
{
    // 优先采用视频流自身时长，它与该流时间基直接对应。
    if ( stream.duration != AV_NOPTS_VALUE && stream.duration > 0 ) {
        // 流级整数时间戳乘以 time_base 转换为秒。
        const double duration =
            static_cast<double>(stream.duration) * av_q2d(stream.time_base);
        // 无穷、NaN 和非正结果不能作为可播放时长。
        if ( std::isfinite(duration) && duration > 0.0 ) return duration;
    }

    // 流级时长缺失时，退回容器以 AV_TIME_BASE 为单位的总时长。
    if ( formatContext.duration != AV_NOPTS_VALUE &&
         formatContext.duration > 0 ) {
        const double duration = static_cast<double>(formatContext.duration) /
                                static_cast<double>(AV_TIME_BASE);
        // 容器时长仍需通过有限性与正值校验。
        if ( std::isfinite(duration) && duration > 0.0 ) return duration;
    }
    // 两级元数据均不可用时返回零，由调用方视为未知时长。
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
        // 每次打开先完整释放旧媒体，失败后实例保持确定的关闭状态。
        close();

        // error_code 重载避免文件系统异常越过项目无异常边界。
        std::error_code filesystemError;
        if ( !std::filesystem::is_regular_file(filePath, filesystemError) ||
             filesystemError ) {
            // 目录、不存在路径和状态查询错误都不是可打开的媒体文件。
            XERROR("VideoFrameDecoder: Video file not found: {}",
                   Config::pathToUtf8(filePath));
            return false;
        }

        // FFmpeg 接口统一接收 UTF-8 路径，避免平台原生编码差异。
        const std::string pathUtf8 = Config::pathToUtf8(filePath);
        AVFormatContext*  rawFormatContext{ nullptr };
        // 原始输出指针立即交给局部 RAII 句柄，后续任一失败都会自动清理。
        int result = avformat_open_input(
            &rawFormatContext, pathUtf8.c_str(), nullptr, nullptr);
        FormatContextPtr openedFormat(rawFormatContext);
        if ( result < 0 || !openedFormat ) {
            // 同时检查错误码和输出指针，防御后端返回不一致状态。
            XERROR("VideoFrameDecoder: Failed to open {}: {}",
                   pathUtf8,
                   ffmpegErrorText(result));
            return false;
        }

        // 完整读取流信息后才能可靠选择视频流、时间基和容器时长。
        result = avformat_find_stream_info(openedFormat.get(), nullptr);
        if ( result < 0 ) {
            XERROR("VideoFrameDecoder: Failed to read streams from {}: {}",
                   pathUtf8,
                   ffmpegErrorText(result));
            return false;
        }

        // 让 FFmpeg 依据流可解码性选择最佳视频流并返回匹配解码器。
        const AVCodec* decoder{ nullptr };
        const int      openedStreamIndex = av_find_best_stream(
            openedFormat.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if ( openedStreamIndex < 0 || !decoder ) {
            // 没有可解码视频时拒绝仅音频或损坏容器。
            XERROR("VideoFrameDecoder: No decodable video stream in {}: {}",
                   pathUtf8,
                   ffmpegErrorText(openedStreamIndex));
            return false;
        }

        // 流索引来自 FFmpeg，仍校验流对象和 codecpar 后再解引用。
        AVStream* openedStream =
            openedFormat->streams[static_cast<unsigned int>(openedStreamIndex)];
        if ( !openedStream || !openedStream->codecpar ) {
            XERROR("VideoFrameDecoder: Invalid video stream in {}", pathUtf8);
            return false;
        }

        // 为所选解码器分配独立上下文，尚不修改实例的既有成员。
        CodecContextPtr openedCodec(avcodec_alloc_context3(decoder));
        if ( !openedCodec ) {
            XERROR("VideoFrameDecoder: Failed to allocate decoder for {}",
                   pathUtf8);
            return false;
        }

        // 把容器流参数复制到解码上下文，保留编解码器特定配置。
        result = avcodec_parameters_to_context(openedCodec.get(),
                                               openedStream->codecpar);
        if ( result < 0 ) {
            XERROR("VideoFrameDecoder: Failed to configure decoder for {}: {}",
                   pathUtf8,
                   ffmpegErrorText(result));
            return false;
        }

        // 解码器打开可能加载编解码器资源，因此仍处于局部事务阶段。
        result = avcodec_open2(openedCodec.get(), decoder, nullptr);
        if ( result < 0 ) {
            XERROR("VideoFrameDecoder: Failed to initialize decoder for {}: {}",
                   pathUtf8,
                   ffmpegErrorText(result));
            return false;
        }

        // 宽高必须为正，后续 RGBA 容量计算和 swscale 都依赖该不变量。
        if ( openedCodec->width <= 0 || openedCodec->height <= 0 ) {
            XERROR("VideoFrameDecoder: Invalid video dimensions in {}: {}x{}",
                   pathUtf8,
                   openedCodec->width,
                   openedCodec->height);
            return false;
        }

        // packet、源帧和转换目标帧在实例生命周期内循环复用。
        PacketPtr openedPacket(av_packet_alloc());
        FramePtr  openedFrame(av_frame_alloc());
        FramePtr  openedConvertedFrame(av_frame_alloc());
        if ( !openedPacket || !openedFrame || !openedConvertedFrame ) {
            // 任一工作缓冲分配失败都回滚整次打开事务。
            XERROR(
                "VideoFrameDecoder: Failed to allocate decode buffers for {}",
                pathUtf8);
            return false;
        }

        // 公开信息只保存稳定值，不暴露 FFmpeg 流与上下文指针。
        VideoInfo openedInfo;
        openedInfo.width  = static_cast<std::uint32_t>(openedCodec->width);
        openedInfo.height = static_cast<std::uint32_t>(openedCodec->height);
        openedInfo.duration =
            resolveDurationSeconds(*openedFormat, *openedStream);

        // 所有验证完成后一次性把局部资源提交到实例，保证失败不留半开状态。
        formatContext  = std::move(openedFormat);
        codecContext   = std::move(openedCodec);
        packet         = std::move(openedPacket);
        decodedFrame   = std::move(openedFrame);
        convertedFrame = std::move(openedConvertedFrame);
        streamIndex    = openedStreamIndex;
        videoInfo      = openedInfo;
        mediaPath      = pathUtf8;
        // 新媒体必须从无缓存、无 EOF、无上次请求的状态开始。
        resetDecodeState();

        // 成功日志记录解码尺寸和已解析时长，便于诊断容器元数据。
        XINFO("VideoFrameDecoder: Opened {} [{}x{}, {:.3f}s]",
              pathUtf8,
              videoInfo.width,
              videoInfo.height,
              videoInfo.duration);
        return true;
    }

    /// @brief 释放当前视频与全部解码缓存。
    /// @note 可重复调用；资源按依赖逆序释放后恢复关闭状态。
    void close()
    {
        // 转换器可能引用帧格式信息，优先于帧与解码器释放。
        scaleContext.reset();
        convertedFrame.reset();
        decodedFrame.reset();
        packet.reset();
        // 解码器必须在输入容器之前释放，避免保留流参数引用。
        codecContext.reset();
        formatContext.reset();
        // 对外可见信息和诊断路径与底层资源同步清空。
        streamIndex = -1;
        videoInfo   = {};
        mediaPath.clear();
        // 即使此前已关闭也重置选择状态，使 close 保持幂等。
        resetDecodeState();
    }

    /// @brief 判断核心容器、解码器和视频流是否有效。
    /// @return 资源完整时返回 true。
    bool isOpen() const
    {
        // 核心资源必须全部存在且流索引有效，部分初始化不视为已打开。
        return formatContext && codecContext && packet && decodedFrame &&
               convertedFrame && streamIndex >= 0;
    }

    /// @brief 选取目标时间点最近的帧，并优先选择不晚于目标的帧。
    /// @param seconds 相对视频起点时间。
    /// @return 解码器内部当前帧观察指针；失败时返回 nullptr。
    const VideoFrame* decodeFrameAt(double seconds)
    {
        // 未打开实例和 NaN/无穷时间请求都不能进入 FFmpeg 解码流程。
        if ( !isOpen() || !std::isfinite(seconds) ) return nullptr;

        // 负时间钳制到视频起点，保持 Seek 与顺序解码输入非负。
        double targetSeconds = std::max(0.0, seconds);
        // 已知时长时把越界请求限制到终点前微小量，避免精确 EOF 无帧。
        if ( videoInfo.duration > 0.0 && targetSeconds >= videoInfo.duration ) {
            targetSeconds =
                std::max(0.0, videoInfo.duration - TIMESTAMP_EPSILON_SECONDS);
        }
        // 倒退请求无法复用当前解码游标，必须回到此前关键帧重新解码。
        const bool movesBackward =
            hasLastRequest &&
            targetSeconds + TIMESTAMP_EPSILON_SECONDS < lastRequestSeconds;
        // 首次远端请求和超过一秒的正向跳转通过 Seek 避免逐帧扫描。
        const bool makesLargeForwardJump =
            (!hasLastRequest && targetSeconds > LARGE_FORWARD_SEEK_SECONDS) ||
            (hasLastRequest &&
             targetSeconds - lastRequestSeconds > LARGE_FORWARD_SEEK_SECONDS);

        // Seek 失败时不更新 lastRequest，调用方可再次请求其他时间点。
        if ( (movesBackward || makesLargeForwardJump) &&
             !seekTo(targetSeconds) ) {
            return nullptr;
        }

        // 记录本次归一化目标，后续请求据此选择顺序解码或 Seek。
        hasLastRequest     = true;
        lastRequestSeconds = targetSeconds;

        // 逐帧推进直到遇到目标之后的第一帧或输入流结束。
        while ( true ) {
            if ( hasPendingFrame ) {
                // 上次预读帧已不晚于新目标时，提升为当前候选并继续推进。
                if ( pendingFrame.timestamp <=
                     targetSeconds + TIMESTAMP_EPSILON_SECONDS ) {
                    currentFrame    = std::move(pendingFrame);
                    hasCurrentFrame = true;
                    hasPendingFrame = false;
                    continue;
                }
                // 预读帧仍晚于目标，当前帧就是可用的最近历史帧。
                break;
            }

            // 没有预读帧时从解码器取得下一个显示帧。
            VideoFrame nextFrame;
            if ( !decodeNextFrame(nextFrame) ) break;

            // 不晚于目标的帧持续替换当前候选，最终留下时间上最新的一帧。
            if ( nextFrame.timestamp <=
                 targetSeconds + TIMESTAMP_EPSILON_SECONDS ) {
                currentFrame    = std::move(nextFrame);
                hasCurrentFrame = true;
                continue;
            }

            // 第一帧未来帧单独缓存，下一次相邻请求可以直接复用。
            pendingFrame    = std::move(nextFrame);
            hasPendingFrame = true;
            break;
        }

        // 部分容器 Seek 后返回的首个可解码帧会略晚于目标时间。此时若没有
        // 更早帧可用，采用该最早帧，避免暂停 Seek 永久停留在空画面。
        if ( !hasCurrentFrame && hasPendingFrame ) {
            // Seek 落点晚于目标时没有历史帧可选，只能采用最早解出的未来帧。
            currentFrame    = std::move(pendingFrame);
            hasCurrentFrame = true;
            hasPendingFrame = false;
        }

        // 返回内部缓存观察指针；下一次解码可能移动或覆盖其像素容器。
        return hasCurrentFrame ? &currentFrame : nullptr;
    }

    /// @brief 重置 packet、帧选择和 EOF 状态但保留已打开资源。
    /// @note 不释放容器、解码器和工作缓冲，供 Seek 后继续复用。
    void resetDecodeState()
    {
        // 清除 drain 与 EOF 标志，使解码循环可以重新读取输入包。
        sentDrainPacket    = false;
        reachedEndOfStream = false;
        // 当前帧、未来帧与请求历史都不跨 Seek 或媒体切换保留。
        hasCurrentFrame = false;
        hasPendingFrame = false;
        hasLastRequest  = false;
        // 时间戳回退状态同步归零，首个无 PTS 帧从零开始推算。
        hasLastDecodedTimestamp = false;
        lastRequestSeconds      = 0.0;
        lastDecodedTimestamp    = 0.0;
        // 释放像素逻辑大小但保留 vector 的实现相关容量供赋值复用。
        currentFrame = {};
        pendingFrame = {};
    }

    /// @brief Seek 到目标时间之前的关键帧并清空解码器内部状态。
    /// @param seconds 相对视频起点时间。
    /// @return Seek 成功时返回 true。
    bool seekTo(double seconds)
    {
        // 流观察指针同时验证容器、索引范围和目标 AVStream。
        AVStream* stream = videoStream();
        if ( !stream ) return false;
        // 秒数乘以 AV_TIME_BASE 前检查 int64_t 表示范围，避免转换溢出。
        if ( seconds >
             static_cast<double>(std::numeric_limits<std::int64_t>::max()) /
                 static_cast<double>(AV_TIME_BASE) ) {
            return false;
        }

        // 先把秒转换到统一微秒时间基，再精确重采样到视频流时间基。
        const std::int64_t relativeTimestamp =
            av_rescale_q(static_cast<std::int64_t>(std::llround(
                             seconds * static_cast<double>(AV_TIME_BASE))),
                         AV_TIME_BASE_Q,
                         stream->time_base);
        // 容器存在非零起始时间时，把相对视频时间重新平移到流绝对时间戳。
        const std::int64_t streamStart =
            stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
        const int result = av_seek_frame(formatContext.get(),
                                         streamIndex,
                                         streamStart + relativeTimestamp,
                                         AVSEEK_FLAG_BACKWARD);
        // BACKWARD 保证尽量落在目标之前的关键帧，由后续顺序解码精确选帧。
        if ( result < 0 ) {
            XERROR("VideoFrameDecoder: Failed to seek {} to {:.3f}s: {}",
                   mediaPath,
                   seconds,
                   ffmpegErrorText(result));
            return false;
        }

        // Seek 后解码器内部参考帧与重排队列全部失效，必须显式 flush。
        avcodec_flush_buffers(codecContext.get());
        // 复用的 packet 和 frame 也解除旧媒体位置对应的缓冲引用。
        av_packet_unref(packet.get());
        av_frame_unref(decodedFrame.get());
        resetDecodeState();
        // 无 PTS 回退从目标时间开始，避免 Seek 后时间戳倒退到零。
        lastDecodedTimestamp = seconds;
        return true;
    }

    /// @brief 解码输出序列中的下一帧并转换为 RGBA8。
    /// @param output 接收转换结果。
    /// @return 成功取得一帧时返回 true。
    /// @warning 同步读取和解码路径，可能消费多个非视频包或等待解码器输出。
    bool decodeNextFrame(VideoFrame& output)
    {
        // EOF 状态一旦确认就直接返回，避免反复调用已排空的解码器。
        if ( reachedEndOfStream ) return false;

        // receive/send 循环遵循 FFmpeg 解码状态机，直到得到帧或确定结束。
        while ( true ) {
            // 优先接收已由此前 packet 产生的帧，避免覆盖解码器待取输出。
            const int receiveResult =
                avcodec_receive_frame(codecContext.get(), decodedFrame.get());
            if ( receiveResult == 0 ) {
                // 原生帧立即转换到自有 RGBA 容器，再解除复用 AVFrame 引用。
                const bool converted = convertFrame(*decodedFrame, output);
                av_frame_unref(decodedFrame.get());
                return converted;
            }
            if ( receiveResult == AVERROR_EOF ) {
                // 解码器 drain 完成后标记终态，后续请求不再读取容器。
                reachedEndOfStream = true;
                return false;
            }
            if ( receiveResult != AVERROR(EAGAIN) ) {
                // 非 EAGAIN 错误表示解码器无法通过继续送包恢复。
                XERROR("VideoFrameDecoder: Failed to receive frame from {}: {}",
                       mediaPath,
                       ffmpegErrorText(receiveResult));
                reachedEndOfStream = true;
                return false;
            }

            // 已送过 drain 包后仍要求更多输入，说明输出已完全消费。
            if ( sentDrainPacket ) {
                reachedEndOfStream = true;
                return false;
            }

            // 读取输入包并跳过其他音频、字幕或附件流的数据包。
            int readResult{ 0 };
            do {
                // 每次读取前释放上一个包引用，复用同一个 AVPacket 结构。
                av_packet_unref(packet.get());
                readResult = av_read_frame(formatContext.get(), packet.get());
            } while ( readResult >= 0 && packet->stream_index != streamIndex );

            if ( readResult == AVERROR_EOF ) {
                // 容器 EOF 后发送空包，要求解码器输出内部延迟帧。
                const int drainResult =
                    avcodec_send_packet(codecContext.get(), nullptr);
                if ( drainResult < 0 && drainResult != AVERROR_EOF ) {
                    // drain 失败无法再得到完整帧序列，进入终止状态。
                    XERROR("VideoFrameDecoder: Failed to drain {}: {}",
                           mediaPath,
                           ffmpegErrorText(drainResult));
                    reachedEndOfStream = true;
                    return false;
                }
                // 记录已发送以确保空包只发送一次，然后回到 receive 阶段。
                sentDrainPacket = true;
                continue;
            }
            if ( readResult < 0 ) {
                // EOF 以外的容器读取错误不可恢复，停止本媒体的后续解码。
                XERROR("VideoFrameDecoder: Failed to read {}: {}",
                       mediaPath,
                       ffmpegErrorText(readResult));
                reachedEndOfStream = true;
                return false;
            }

            // 把当前视频包交给解码器，输出将在下一次循环顶部接收。
            const int sendResult =
                avcodec_send_packet(codecContext.get(), packet.get());
            // 解码器已取得所需引用后立即释放复用包，避免积累容器缓冲。
            av_packet_unref(packet.get());
            if ( sendResult < 0 ) {
                // 单包不可解码时记录并继续读取，允许损坏流恢复到后续关键帧。
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
    /// @warning 会按帧尺寸调整 RGBA vector，可能在分辨率变化时分配内存。
    bool convertFrame(const AVFrame& sourceFrame, VideoFrame& output)
    {
        // swscale 需要正尺寸和明确像素格式才能建立转换参数。
        const int width  = sourceFrame.width;
        const int height = sourceFrame.height;
        if ( width <= 0 || height <= 0 ||
             sourceFrame.format == AV_PIX_FMT_NONE ) {
            return false;
        }

        // 转为 size_t 后先验证 width * height * 4 的容量计算不会溢出。
        const std::size_t unsignedWidth  = static_cast<std::size_t>(width);
        const std::size_t unsignedHeight = static_cast<std::size_t>(height);
        if ( unsignedWidth >
             std::numeric_limits<std::size_t>::max() / unsignedHeight / 4U ) {
            // 拒绝无法安全分配连续 RGBA8 缓冲的异常帧尺寸。
            return false;
        }

        // 延迟创建转换器，未实际解出视频帧时不承担 swscale 初始化成本。
        if ( !scaleContext ) {
            ScaleContextPtr allocatedContext(sws_alloc_context());
            if ( allocatedContext ) {
                // 双线性缩放标志同时适用于像素格式转换中的采样过程。
                allocatedContext->flags = SWS_BILINEAR;
            }
            scaleContext = std::move(allocatedContext);
        }
        if ( !scaleContext ) {
            // 转换器分配失败时保持实例可关闭，但当前帧无法输出。
            XERROR("VideoFrameDecoder: Failed to create RGBA converter for {}",
                   mediaPath);
            return false;
        }

        // 输出像素紧密排列为 width * height 个 RGBA8 像素。
        output.rgba.resize(unsignedWidth * unsignedHeight * 4U);
        // 目标 AVFrame 只描述 output.rgba，不拥有该 vector 的像素内存。
        av_frame_unref(convertedFrame.get());
        convertedFrame->format = AV_PIX_FMT_RGBA;
        convertedFrame->width  = width;
        convertedFrame->height = height;
        // 目标格式明确标为 RGBA 和全范围 RGB，供 swscale 写出显示像素。
        convertedFrame->colorspace  = AVCOL_SPC_RGB;
        convertedFrame->color_range = AVCOL_RANGE_JPEG;
        // 源帧色彩原色、传递曲线和像素宽高比继续传给转换上下文。
        convertedFrame->color_primaries     = sourceFrame.color_primaries;
        convertedFrame->color_trc           = sourceFrame.color_trc;
        convertedFrame->sample_aspect_ratio = sourceFrame.sample_aspect_ratio;
        // 单平面 RGBA 数据首地址指向 vector，行跨度固定为 width * 4。
        convertedFrame->data[0]       = output.rgba.data();
        convertedFrame->linesize[0]   = width * 4;
        convertedFrame->extended_data = convertedFrame->data;

        // sws_scale_frame 根据源帧逐帧色彩属性选择转换矩阵并写入目标缓冲。
        const int scaleResult = sws_scale_frame(
            scaleContext.get(), convertedFrame.get(), &sourceFrame);
        av_frame_unref(convertedFrame.get());
        if ( scaleResult < 0 ) {
            // 转换失败时清空全部输出字段，禁止调用方使用部分写入的像素。
            XERROR("VideoFrameDecoder: Failed to convert frame from {}: {}",
                   mediaPath,
                   ffmpegErrorText(scaleResult));
            output = {};
            return false;
        }

        // 仅在像素完整转换后提交尺寸和时间戳，形成一致 VideoFrame。
        output.width     = static_cast<std::uint32_t>(width);
        output.height    = static_cast<std::uint32_t>(height);
        output.timestamp = resolveFrameTimestamp(sourceFrame);
        return true;
    }

    /// @brief 依据 best_effort_timestamp 解析 VFR 帧显示时间。
    /// @param sourceFrame 解码器输出帧。
    /// @return 相对视频起点的非负秒数。
    /// @note 缺少 PTS 时依据估算帧率从上一帧时间向前推进。
    double resolveFrameTimestamp(const AVFrame& sourceFrame)
    {
        // 当前流决定时间基、起始时间与回退帧率。
        AVStream* stream = videoStream();
        if ( !stream ) return 0.0;

        // best_effort_timestamp 已考虑重排，优先于原始 PTS 用于显示顺序。
        std::int64_t timestamp = sourceFrame.best_effort_timestamp;
        // 部分解码器不提供 best effort 值，此时退回原始显示时间戳。
        if ( timestamp == AV_NOPTS_VALUE ) timestamp = sourceFrame.pts;

        // 完全缺失时间戳时默认保持上次结果，后续可按估算帧率推进。
        double seconds = lastDecodedTimestamp;
        if ( timestamp != AV_NOPTS_VALUE ) {
            // 扣除流起始时间，把容器绝对时间戳转换为相对视频时间。
            const std::int64_t streamStart =
                stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
            seconds = static_cast<double>(timestamp - streamStart) *
                      av_q2d(stream->time_base);
        } else if ( hasLastDecodedTimestamp ) {
            // 只有已有基准时间时才使用估算帧率生成下一帧时间。
            const AVRational guessedRate =
                av_guess_frame_rate(formatContext.get(), stream, nullptr);
            if ( guessedRate.num > 0 && guessedRate.den > 0 ) {
                // 帧率倒数是相邻显示帧的估算秒间隔。
                seconds += av_q2d(av_inv_q(guessedRate));
            }
        }

        // 非有限时间戳回退到零，负容器时间钳制到视频相对起点。
        if ( !std::isfinite(seconds) ) seconds = 0.0;
        seconds              = std::max(0.0, seconds);
        lastDecodedTimestamp = seconds;
        // 即使使用回退值也建立后续无 PTS 帧可继续推进的基准。
        hasLastDecodedTimestamp = true;
        return seconds;
    }

    /// @brief 获取当前视频流观察指针。
    /// @return 流索引有效时返回视频流，否则返回空指针。
    AVStream* videoStream() const
    {
        // 同时验证容器存在、索引非负且未超出当前流数组范围。
        if ( !formatContext || streamIndex < 0 ||
             static_cast<unsigned int>(streamIndex) >=
                 formatContext->nb_streams ) {
            return nullptr;
        }
        // AVStream 所有权仍属于 formatContext，调用方只在实例内部短暂观察。
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

/// @brief 创建处于关闭状态的解码器并分配私有实现对象。
VideoFrameDecoder::VideoFrameDecoder() : m_impl(std::make_unique<Impl>()) {}

/// @brief 在 FFmpeg 类型完整可见的位置销毁私有实现和媒体资源。
VideoFrameDecoder::~VideoFrameDecoder() = default;

/// @brief 以事务方式打开视频并替换当前媒体。
/// @param filePath 本地视频文件路径。
/// @return 底层容器、视频流和解码器全部初始化成功时返回 true。
bool VideoFrameDecoder::open(const std::filesystem::path& filePath)
{
    // 公共接口委托给 PImpl，头文件无需暴露 FFmpeg 类型。
    return m_impl->open(filePath);
}

/// @brief 关闭当前视频并恢复全零公开信息。
void VideoFrameDecoder::close()
{
    // Impl::close 可重复调用，析构前显式关闭不会造成重复释放。
    m_impl->close();
}

/// @brief 查询实例是否拥有完整可用的 FFmpeg 解码状态。
/// @return 容器、解码器、工作缓冲和视频流均有效时返回 true。
bool VideoFrameDecoder::isOpen() const
{
    return m_impl->isOpen();
}

/// @brief 返回当前媒体基础信息。
/// @return PImpl 内稳定的 VideoInfo 引用；关闭状态下字段均为零。
const VideoInfo& VideoFrameDecoder::info() const
{
    // 引用生命周期与解码器实例一致，但下一次 open 或 close 会覆盖内容。
    return m_impl->videoInfo;
}

/// @brief 解码目标时间点应显示的最近 RGBA8 帧。
/// @param seconds 相对视频起点的目标秒数。
/// @return 内部帧观察指针；无有效帧时返回 nullptr。
/// @warning 同步读取、Seek 和像素转换入口，不得从渲染命令录制热路径调用。
const VideoFrame* VideoFrameDecoder::decodeFrameAt(double seconds)
{
    // 返回指针由 Impl 缓存拥有，下次时间请求可能使其内容失效。
    return m_impl->decodeFrameAt(seconds);
}

/// @brief 使用短生命周期解码器探测本地视频尺寸和时长。
/// @param filePath 待探测的视频路径。
/// @return 打开成功时复制 VideoInfo，否则返回空。
/// @warning 会同步访问文件和初始化解码器，只能用于低频加载流程。
std::optional<VideoInfo> probeVideoInfo(const std::filesystem::path& filePath)
{
    // 复用正式打开路径，保证探测结果与实际解码器接受条件一致。
    VideoFrameDecoder decoder;
    // 打开失败时不返回部分容器信息。
    if ( !decoder.open(filePath) ) return std::nullopt;
    // optional 按值复制基础信息，局部解码器销毁后结果仍有效。
    return decoder.info();
}

}  // namespace MMM::Utils
