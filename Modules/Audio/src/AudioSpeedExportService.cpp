#include "audio/AudioSpeedExportService.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <ice/config/config.hpp>
#include <ice/core/IAudioNode.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/core/effect/TimeStretcher.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>
#include <ice/out/io/FFmpegFileReceiver.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <memory>
#include <string>
#include <utility>

namespace MMM::Audio
{
namespace
{

/// @brief 音频倍速导出的离线处理块大小。
constexpr std::size_t AUDIO_SPEED_EXPORT_CHUNK_FRAMES = 65536;

/// @brief 发送导出进度。
/// @param options 导出参数。
/// @param progress 进度值。
/// @param message 进度文本。
void emitProgress(const AudioSpeedExportOptions& options, float progress,
                  std::string message)
{
    if ( !options.progressCallback ) return;
    options.progressCallback(AudioSpeedExportProgress{
        std::clamp(progress, 0.0f, 1.0f), std::move(message) });
}

/// @brief 不保留音高的倍速采样节点。
class PitchShiftSpeedNode : public ice::IAudioNode
{
public:
    /// @brief 构造采样节点。
    /// @param track 输入音轨。
    /// @param speed 倍速倍率。
    /// @param format 引擎内部音频格式。
    PitchShiftSpeedNode(std::shared_ptr<ice::AudioTrack> track, double speed,
                        ice::AudioDataFormat format)
        : m_track(std::move(track)), m_speed(speed), m_format(format)
    {
    }

    /// @brief 拉取并写入一块变调倍速音频。
    /// @param buffer 输出缓冲。
    /// @warning 离线导出路径：由 FFmpegFileReceiver
    /// 批量调用；会按块读取音轨缓存并做线性插值，不属于实时播放线程。
    void process(ice::AudioBuffer& buffer) override
    {
        buffer.clear();
        if ( !m_track || m_speed <= 0.0 || !std::isfinite(m_speed) ) {
            return;
        }

        const std::size_t outputFrames = buffer.num_frames();
        const std::size_t trackFrames  = m_track->num_frames();
        if ( outputFrames == 0 || trackFrames == 0 ||
             m_sourcePosition >= static_cast<double>(trackFrames) ) {
            m_sourcePosition += static_cast<double>(outputFrames) * m_speed;
            return;
        }

        const double lastSourcePosition =
            m_sourcePosition + static_cast<double>(outputFrames - 1) * m_speed;
        const std::size_t firstSourceFrame =
            static_cast<std::size_t>(std::floor(m_sourcePosition));
        const std::size_t lastSourceFrame = std::min(
            trackFrames - 1,
            static_cast<std::size_t>(std::floor(lastSourcePosition)) + 1);
        const std::size_t sourceFrameCount =
            lastSourceFrame >= firstSourceFrame
                ? lastSourceFrame - firstSourceFrame + 1
                : std::size_t{ 0 };
        if ( sourceFrameCount == 0 ) {
            m_sourcePosition += static_cast<double>(outputFrames) * m_speed;
            return;
        }

        m_sourceBuffer.resize(m_format, sourceFrameCount);
        m_sourceBuffer.clear();
        const std::size_t readFrames =
            m_track->read(m_sourceBuffer, firstSourceFrame, sourceFrameCount);
        if ( readFrames < sourceFrameCount ) {
            m_sourceBuffer.clear_from(readFrames);
        }

        const float* const* input  = m_sourceBuffer.raw_ptrs();
        float**             output = buffer.raw_ptrs();
        if ( !input || !output ) {
            m_sourcePosition += static_cast<double>(outputFrames) * m_speed;
            return;
        }

        for ( std::size_t frame = 0; frame < outputFrames; ++frame ) {
            const double sourcePosition =
                m_sourcePosition + static_cast<double>(frame) * m_speed;
            if ( sourcePosition >= static_cast<double>(trackFrames) ) {
                continue;
            }

            const std::size_t sourceIndex =
                static_cast<std::size_t>(std::floor(sourcePosition));
            const std::size_t nextSourceIndex =
                std::min(sourceIndex + 1, trackFrames - 1);
            const double fraction =
                sourcePosition - static_cast<double>(sourceIndex);
            const std::size_t localA = sourceIndex - firstSourceFrame;
            const std::size_t localB = nextSourceIndex - firstSourceFrame;

            for ( uint16_t channel = 0; channel < m_format.channels;
                  ++channel ) {
                const float a =
                    localA < readFrames ? input[channel][localA] : 0.0f;
                const float b =
                    localB < readFrames ? input[channel][localB] : 0.0f;
                output[channel][frame] =
                    static_cast<float>(a + (b - a) * fraction);
            }
        }

        m_sourcePosition += static_cast<double>(outputFrames) * m_speed;
    }

private:
    std::shared_ptr<ice::AudioTrack> m_track;
    double                           m_speed{ 1.0 };
    ice::AudioDataFormat             m_format;
    ice::AudioBuffer                 m_sourceBuffer;
    double                           m_sourcePosition{ 0.0 };
};

/// @brief 计算导出目标帧数。
/// @param inputFrames 输入音频帧数。
/// @param sampleRate 输出采样率。
/// @param options 导出参数。
/// @return 输出帧数。
std::size_t calculateTargetFrames(std::size_t                    inputFrames,
                                  std::uint32_t                  sampleRate,
                                  const AudioSpeedExportOptions& options)
{
    const auto speedFrames = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(
            std::ceil(static_cast<double>(inputFrames) / options.speed)));
    if ( options.minimumDurationSeconds <= 0.0 ||
         !std::isfinite(options.minimumDurationSeconds) ) {
        return speedFrames;
    }

    const auto minimumFrames = static_cast<std::size_t>(std::ceil(
        options.minimumDurationSeconds * static_cast<double>(sampleRate)));
    return std::max(speedFrames, minimumFrames);
}

/// @brief 创建保留音高的音频图。
/// @param track 输入音轨。
/// @param speed 倍速倍率。
/// @return 图的输出节点。
std::shared_ptr<ice::IAudioNode> createPitchPreservedGraph(
    const std::shared_ptr<ice::AudioTrack>& track, double speed)
{
    auto source = std::make_shared<ice::SourceNode>(track);
    source->setvolume(1.0f);
    source->play();

    auto stretcher = std::make_shared<ice::TimeStretcher>();
    stretcher->set_inputnode(source);
    stretcher->set_playback_ratio(speed);
    stretcher->set_pitch_semitones(0.0);
    return stretcher;
}

/// @brief 创建不保留音高的音频图。
/// @param track 输入音轨。
/// @param speed 倍速倍率。
/// @param format 引擎内部音频格式。
/// @return 图的输出节点。
std::shared_ptr<ice::IAudioNode> createPitchShiftedGraph(
    const std::shared_ptr<ice::AudioTrack>& track, double speed,
    const ice::AudioDataFormat& format)
{
    return std::make_shared<PitchShiftSpeedNode>(track, speed, format);
}

}  // namespace

AudioSpeedExportResult AudioSpeedExportService::exportWav(
    const AudioSpeedExportOptions& options)
{
    AudioSpeedExportResult result;
    if ( options.speed <= 0.0 || !std::isfinite(options.speed) ) {
        result.errorMessage = "Invalid speed multiplier";
        return result;
    }
    if ( options.inputPath.empty() || options.outputPath.empty() ) {
        result.errorMessage = "Input or output audio path is empty";
        return result;
    }

    std::error_code existsError;
    if ( !std::filesystem::is_regular_file(options.inputPath, existsError) ||
         existsError ) {
        result.errorMessage = "Input audio file does not exist";
        return result;
    }

    emitProgress(options, 0.0f, "正在打开音频...");

    std::unique_ptr<ice::ThreadPool> fallbackThreadPool;
    ice::ThreadPool* threadPool = Runtime::AppThreadPool::instance().get();
    if ( !threadPool ) {
        fallbackThreadPool = std::make_unique<ice::ThreadPool>(1);
        threadPool         = fallbackThreadPool.get();
    }

    auto decoderFactory = std::make_shared<ice::FFmpegDecoderFactory>();
    auto track = ice::AudioTrack::create(Config::pathToUtf8(options.inputPath),
                                         *threadPool,
                                         decoderFactory,
                                         ice::CachingStrategy::CACHY);
    if ( !track ) {
        result.errorMessage = "Failed to decode input audio";
        return result;
    }

    const ice::AudioDataFormat format = ice::ICEConfig::internal_format;
    if ( format.channels == 0 || format.samplerate == 0 ) {
        result.errorMessage = "Invalid internal audio format";
        return result;
    }

    emitProgress(options, 0.05f, "正在载入音频缓存...");
    const std::size_t inputFrames = track->num_frames();
    if ( inputFrames == 0 ) {
        result.errorMessage = "Input audio is empty";
        return result;
    }

    const std::size_t targetFrames =
        calculateTargetFrames(inputFrames, format.samplerate, options);
    auto graph = options.preservePitch
                     ? createPitchPreservedGraph(track, options.speed)
                     : createPitchShiftedGraph(track, options.speed, format);
    if ( !graph ) {
        result.errorMessage = "Failed to create audio export graph";
        return result;
    }

    emitProgress(options, 0.08f, "正在通过音频引擎导出...");

    ice::FFmpegFileReceiver receiver(options.outputPath, format);
    receiver.set_source(graph);
    receiver.set_target_frames(targetFrames);
    receiver.set_block_frames(AUDIO_SPEED_EXPORT_CHUNK_FRAMES);
    receiver.set_progress_callback(
        [&options, targetFrames](std::size_t frames) {
            const float progress =
                0.08f +
                0.90f * static_cast<float>(frames) /
                    static_cast<float>(std::max<std::size_t>(1, targetFrames));
            emitProgress(options, progress, "正在编码倍速音频...");
        });

    if ( !receiver.start() ) {
        result.errorMessage = receiver.error_message().empty()
                                  ? "Failed to encode output audio"
                                  : receiver.error_message();
        return result;
    }

    result.outputFrames          = receiver.frames_written();
    result.outputDurationSeconds = static_cast<double>(result.outputFrames) /
                                   static_cast<double>(format.samplerate);
    emitProgress(options, 1.0f, "倍速音频导出完成");
    XINFO("AudioSpeedExportService: wrote {} frames to {}",
          result.outputFrames,
          Config::pathToUtf8(options.outputPath));
    result.success = true;
    return result;
}

}  // namespace MMM::Audio
