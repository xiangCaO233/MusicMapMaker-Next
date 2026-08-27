#include "audio/AudioOriginAlignmentService.h"

#include "audio/AudioTimelineExportService.h"
#include "config/Utf8Path.h"

#include <cmath>

namespace MMM::Audio
{

AudioOriginAlignmentResult AudioOriginAlignmentService::alignToOrigin(
    const AudioOriginAlignmentOptions& options)
{
    AudioOriginAlignmentResult result;
    if ( options.inputPath.empty() || options.outputPath.empty() ) {
        result.errorMessage = "音频原点对齐的输入或输出路径为空";
        return result;
    }
    if ( !std::isfinite(options.phaseMilliseconds) ) {
        result.errorMessage = "音频原点对齐相位无效";
        return result;
    }

    AudioTrackConfig unityConfig;
    unityConfig.volume        = 1.0F;
    unityConfig.playbackSpeed = 1.0F;
    unityConfig.playbackPitch = 0.0F;
    unityConfig.muted         = false;
    unityConfig.eqEnabled     = false;

    AudioTimelineExportOptions timelineOptions;
    timelineOptions.events.push_back(AudioTimelineLoadEvent{
        .eventId               = 1U,
        .resourceKey           = "mcz-main-audio-origin-alignment",
        .filePath              = Config::pathToUtf8(options.inputPath),
        .effectiveStartSeconds = -options.phaseMilliseconds / 1000.0,
        .bgmTrackIndex         = 0U,
        .eventVolume           = 1.0F,
        .resourceConfig        = std::move(unityConfig),
    });
    timelineOptions.outputPath = options.outputPath;

    const auto timelineResult =
        AudioTimelineExportService::exportMixedAudio(timelineOptions);
    result.success      = timelineResult.success;
    result.errorMessage = timelineResult.errorMessage;
    result.outputFrames = timelineResult.outputFrames;
    return result;
}

}  // namespace MMM::Audio
