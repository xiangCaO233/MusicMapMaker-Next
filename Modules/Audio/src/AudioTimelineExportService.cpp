#include "audio/AudioTimelineExportService.h"

#include "audio/AudioTimelineMixerNode.h"
#include "audio/AudioTimelineResourceProcessor.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "runtime/AppThreadPool.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ice/config/config.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/manage/dec/ffmpeg/FFmpegDecoderFactory.hpp>
#include <ice/out/io/FFmpegFileReceiver.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MMM::Audio
{
namespace
{

/// @brief 离线复合时间线单次处理的最大帧数。
constexpr std::size_t AUDIO_TIMELINE_EXPORT_CHUNK_FRAMES = 65536U;

/// @brief 将秒数安全转换为统一时间线帧。
/// @param seconds 秒数，允许为负。
/// @return 限制在 AudioTimelineFrame 可表达范围内的最近帧。
AudioTimelineFrame secondsToTimelineFrame(double seconds) noexcept
{
    if ( !std::isfinite(seconds) ) return 0;

    const long double frames =
        static_cast<long double>(seconds) *
        static_cast<long double>(ice::ICEConfig::internal_format.samplerate);
    constexpr auto MIN_FRAME = static_cast<long double>(
        std::numeric_limits<AudioTimelineFrame>::min());
    constexpr auto MAX_FRAME = static_cast<long double>(
        std::numeric_limits<AudioTimelineFrame>::max());
    if ( frames <= MIN_FRAME ) {
        return std::numeric_limits<AudioTimelineFrame>::min();
    }
    if ( frames >= MAX_FRAME ) {
        return std::numeric_limits<AudioTimelineFrame>::max();
    }
    return static_cast<AudioTimelineFrame>(std::llround(frames));
}

/// @brief 规范化项目音频资源音量。
/// @param volume 持久化线性音量。
/// @return 有限的零到一音量。
float sanitizedResourceVolume(float volume) noexcept
{
    return std::isfinite(volume) ? std::clamp(volume, 0.0F, 1.0F) : 0.0F;
}

/// @brief 规范化单个事件音量。
/// @param volume 事件线性音量。
/// @return 有限的非负音量。
float sanitizedEventVolume(float volume) noexcept
{
    return std::isfinite(volume) ? std::max(volume, 0.0F) : 0.0F;
}

}  // namespace

AudioTimelineExportResult AudioTimelineExportService::exportMixedAudio(
    const AudioTimelineExportOptions& options)
{
    AudioTimelineExportResult result;
    if ( options.outputPath.empty() ) {
        result.errorMessage = "音频输出路径为空";
        return result;
    }

    const ice::AudioDataFormat format = ice::ICEConfig::internal_format;
    if ( format.channels == 0U || format.samplerate == 0U ) {
        result.errorMessage = "音频引擎内部格式无效";
        return result;
    }

    ice::ThreadPool* threadPool = Runtime::AppThreadPool::instance().get();
    if ( !threadPool ) {
        result.errorMessage = "音频后台线程池尚未初始化";
        return result;
    }

    std::error_code filesystemError;
    if ( !options.outputPath.parent_path().empty() ) {
        std::filesystem::create_directories(options.outputPath.parent_path(),
                                            filesystemError);
        if ( filesystemError ) {
            result.errorMessage = "无法创建音频输出目录";
            return result;
        }
    }

    auto decoderFactory = std::make_shared<ice::FFmpegDecoderFactory>();
    std::unordered_map<std::string, std::shared_ptr<ice::AudioTrack>>
        tracksByPath;
    tracksByPath.reserve(options.events.size());
    for ( const auto& event : options.events ) {
        if ( event.filePath.empty() ) {
            result.errorMessage = "存在未解析的音频资源：" + event.resourceKey;
            return result;
        }
        if ( tracksByPath.contains(event.filePath) ) continue;

        const auto path = Config::utf8ToPath(event.filePath);
        filesystemError.clear();
        if ( !std::filesystem::is_regular_file(path, filesystemError) ||
             filesystemError ) {
            result.errorMessage = "找不到音频资源：" + event.filePath;
            return result;
        }
        auto track = ice::AudioTrack::create(event.filePath,
                                             *threadPool,
                                             decoderFactory,
                                             ice::CachingStrategy::CACHY);
        if ( !track || track->num_frames() == 0U ) {
            result.errorMessage = "无法解码音频资源：" + event.filePath;
            return result;
        }
        tracksByPath.emplace(event.filePath, std::move(track));
    }

    std::unordered_map<std::string,
                       std::shared_ptr<const PreparedTimelineAudio>>
        preparedByConfig;
    preparedByConfig.reserve(options.events.size());
    std::vector<PreparedTimelineClip> clips;
    clips.reserve(options.events.size());
    for ( const auto& event : options.events ) {
        if ( !std::isfinite(event.effectiveStartSeconds) ) {
            result.errorMessage = "音频事件起播时间无效：" + event.resourceKey;
            return result;
        }

        const auto trackIterator = tracksByPath.find(event.filePath);
        if ( trackIterator == tracksByPath.end() || !trackIterator->second ) {
            result.errorMessage = "音频资源未完成载入：" + event.filePath;
            return result;
        }

        const auto processingKey = makeAudioResourceProcessingCacheKey(
            event.filePath, event.resourceConfig);
        auto preparedIterator = preparedByConfig.find(processingKey);
        if ( preparedIterator == preparedByConfig.end() ) {
            auto prepared = prepareAudioTimelineResource(trackIterator->second,
                                                         event.resourceConfig);
            if ( !prepared ) {
                result.errorMessage = "无法处理音频资源：" + event.filePath;
                return result;
            }
            preparedIterator =
                preparedByConfig.emplace(processingKey, std::move(prepared))
                    .first;
        }

        const float resourceVolume =
            event.resourceConfig.muted
                ? 0.0F
                : sanitizedResourceVolume(event.resourceConfig.volume);
        clips.push_back(PreparedTimelineClip{
            .eventId    = event.eventId,
            .sourceKey  = event.resourceKey,
            .startFrame = secondsToTimelineFrame(event.effectiveStartSeconds),
            .bgmTrackIndex = event.bgmTrackIndex,
            .volume = resourceVolume * sanitizedEventVolume(event.eventVolume),
            .audio  = preparedIterator->second,
        });
    }

    const double chartEndSeconds = std::isfinite(options.chartEndSeconds)
                                       ? std::max(options.chartEndSeconds, 0.0)
                                       : 0.0;
    auto         timeline        = std::make_shared<AudioTimelineMixerNode>(
        std::move(clips),
        secondsToTimelineFrame(chartEndSeconds),
        AUDIO_TIMELINE_EXPORT_CHUNK_FRAMES);
    timeline->play();

    const auto timelineEndFrame = timeline->timelineEndFrame();
    const auto positiveEndFrame =
        timelineEndFrame > 0 ? static_cast<std::uint64_t>(timelineEndFrame)
                             : std::uint64_t{ 1U };
    const auto targetFrames = static_cast<std::size_t>(std::min(
        positiveEndFrame,
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())));

    ice::FFmpegFileReceiver receiver(options.outputPath, format);
    receiver.set_source(timeline);
    receiver.set_target_frames(targetFrames);
    receiver.set_block_frames(AUDIO_TIMELINE_EXPORT_CHUNK_FRAMES);
    if ( !receiver.start() ) {
        result.errorMessage = receiver.error_message().empty()
                                  ? "无法编码拼装后的音频"
                                  : receiver.error_message();
        return result;
    }

    result.outputFrames = receiver.frames_written();
    result.success      = result.outputFrames > 0U;
    if ( !result.success ) {
        result.errorMessage = "音频编码器未写出任何数据";
        return result;
    }

    XINFO("AudioTimelineExportService: wrote {} frames to {}",
          result.outputFrames,
          Config::pathToUtf8(options.outputPath));
    return result;
}

}  // namespace MMM::Audio
