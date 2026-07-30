#include "log/colorful-log.h"
#include "logic/session/PlaybackController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"

#include <cmath>
#include <memory>

namespace
{

/// @brief 使用小容差比较播放时间。
bool near(double lhs, double rhs)
{
    return std::abs(lhs - rhs) < 1e-6;
}

/// @brief 验证自然结束标志会在下一次播放请求前先回到零点。
bool testFinishedTimelineRewindsBeforeActivation()
{
    MMM::Logic::SessionContext     context;
    MMM::Logic::PlaybackController controller(context);
    context.isActiveSession                   = true;
    context.currentTime                       = 37.5;
    context.restartPlaybackAfterFinishPending = true;

    controller.handleCommand(MMM::Logic::CmdSetPlayState{ true });
    if ( context.isPlaying || !near(context.currentTime, 0.0) ||
         context.restartPlaybackAfterFinishPending ) {
        XERROR("Finished timeline did not rewind before activation");
        return false;
    }
    return true;
}

/// @brief 验证拉伸器尾音期间暂停不会把视觉时间冻结到谱面末尾之外。
bool testPauseClampsVisualClockToTimelineEnd()
{
    MMM::Logic::SessionContext     context;
    MMM::Logic::PlaybackController controller(context);
    context.isActiveSession                           = true;
    context.isPlaying                                 = true;
    context.audioTimelineDescriptor.m_chartEndSeconds = 5.0;
    context.playbackVisualClock.rebase(8.0, 100.0, 1.0, false);

    controller.handleCommand(MMM::Logic::CmdSetPlayState{ false });
    if ( context.isPlaying || !near(context.currentTime, 5.0) ||
         !near(context.playbackVisualClock.currentTimeAt(101.0), 5.0) ) {
        XERROR("Pause preserved a visual time beyond the timeline end");
        return false;
    }
    return true;
}

/// @brief 验证零采样与多采样谱面都生成独立稳定描述符。
bool testZeroAndMultipleSampleDescriptors()
{
    MMM::Project project;
    project.m_projectRoot = "/tmp/mmm-session-timeline-test";

    MMM::Logic::SessionContext context;
    context.currentBeatmap = std::make_shared<MMM::BeatMap>();
    MMM::Timing timing;
    timing.m_timestamp = 500.0;
    context.currentBeatmap->m_timings.push_back(timing);
    if ( !MMM::Logic::SessionUtils::rebuildAudioTimelineDescriptor(context,
                                                                   &project) ||
         !context.audioTimelineDescriptor.m_events.empty() ||
         context.audioTimelineDescriptor.m_fingerprint.empty() ||
         !near(context.audioTimelineDescriptor.m_chartEndSeconds, 0.5) ) {
        XERROR("Zero-sample chart did not build an independent descriptor");
        return false;
    }

    const std::string emptyFingerprint =
        context.audioTimelineDescriptor.m_fingerprint;
    context.currentBeatmap->m_audioSamples.push_back(
        { .m_timestamp = 0.0, .m_audioResourceId = "missing-a" });
    context.currentBeatmap->m_audioSamples.push_back(
        { .m_timestamp = 250.0, .m_audioResourceId = "missing-b" });
    context.isAudioTimelineDescriptorDirty = true;
    if ( !MMM::Logic::SessionUtils::rebuildAudioTimelineDescriptor(context,
                                                                   &project) ||
         context.audioTimelineDescriptor.m_events.size() != 2U ||
         context.audioTimelineDescriptor.m_diagnostics.size() != 2U ||
         context.audioTimelineDescriptor.m_fingerprint == emptyFingerprint ) {
        XERROR("Multiple samples were not preserved in the descriptor");
        return false;
    }

    const std::string multiSampleFingerprint =
        context.audioTimelineDescriptor.m_fingerprint;
    context.isAudioTimelineActivationPending               = false;
    context.currentBeatmap->m_audioSamples.front().m_track = 128U;
    context.isAudioTimelineDescriptorDirty                 = true;
    if ( !MMM::Logic::SessionUtils::rebuildAudioTimelineDescriptor(context,
                                                                   &project) ||
         context.audioTimelineDescriptor.m_fingerprint ==
             multiSampleFingerprint ||
         !context.isAudioTimelineActivationPending ||
         context.audioTimelineDescriptor.m_events.front().bgmTrackIndex !=
             128U ) {
        XERROR("BGM lane routing change did not request an audio reload");
        return false;
    }
    return true;
}

/// @brief 验证不同指纹切换不会继承旧时间或播放态。
bool testTimelineSwitchUsesCompleteFingerprint()
{
    const auto different = MMM::Logic::SessionUtils::resolveAudioTimelineSwitch(
        "timeline-a", "timeline-b", 8.0, 3.0, true, false, true);
    const auto matching = MMM::Logic::SessionUtils::resolveAudioTimelineSwitch(
        "timeline-a", "timeline-a", 8.0, 3.0, true, false, true);
    if ( !near(different.m_targetTime, 3.0) || different.m_resumePlayback ||
         !near(matching.m_targetTime, 8.0) || !matching.m_resumePlayback ) {
        XERROR("Timeline switch inherited state across different fingerprints");
        return false;
    }
    return true;
}

/// @brief 验证 transport 自然结束快照停止会话并武装下次重播。
bool testNaturalFinishSnapshotArmsRestart()
{
    MMM::Logic::SessionContext context;
    context.audioTimelineDescriptor.m_fingerprint = "timeline";
    context.isPlaying                             = true;
    const MMM::Audio::AudioTimelineClockSnapshot snapshot{
        .positionFrame         = 12000,
        .steadyTimeNanoseconds = 100'000'000'000,
        .sampleRate            = 1000U,
        .playbackRate          = 1.0,
        .state            = MMM::Audio::AudioTimelinePlaybackState::Stopped,
        .epoch            = 1U,
        .seekSequence     = 2U,
        .playbackSequence = 2U,
        .sequence         = 2U,
        .finished         = true,
        .valid            = true,
    };
    if ( MMM::Logic::SessionUtils::applyAudioTimelineTransportSnapshot(
             context,
             "timeline",
             snapshot,
             100.0,
             context.lastConfig.settings.syncConfig) ||
         context.isPlaying || !context.restartPlaybackAfterFinishPending ||
         !near(context.currentTime, 12.0) ) {
        XERROR("Natural finish snapshot did not arm playback restart");
        return false;
    }
    return true;
}

/// @brief 验证普通 Stopped 快照不会伪装成自然播放结束。
bool testNonFinishedStopDoesNotArmRestart()
{
    MMM::Logic::SessionContext context;
    context.audioTimelineDescriptor.m_fingerprint = "timeline";
    context.isPlaying                             = true;
    const MMM::Audio::AudioTimelineClockSnapshot snapshot{
        .positionFrame         = 4000,
        .steadyTimeNanoseconds = 100'000'000'000,
        .sampleRate            = 1000U,
        .playbackRate          = 1.0,
        .state            = MMM::Audio::AudioTimelinePlaybackState::Stopped,
        .epoch            = 1U,
        .seekSequence     = 2U,
        .playbackSequence = 2U,
        .sequence         = 2U,
        .finished         = false,
        .valid            = true,
    };
    if ( MMM::Logic::SessionUtils::applyAudioTimelineTransportSnapshot(
             context,
             "timeline",
             snapshot,
             100.0,
             context.lastConfig.settings.syncConfig) ||
         context.isPlaying || context.restartPlaybackAfterFinishPending ) {
        XERROR("Non-finished stop incorrectly armed playback restart");
        return false;
    }
    return true;
}

/// @brief 验证同步 follower 只沿用源画布壁钟，不以离散音频 block 重新校准。
bool testFollowerUsesRebasedSourceClock()
{
    MMM::Logic::SessionContext context;
    context.audioTimelineDescriptor.m_fingerprint = "timeline";
    context.isAudioTimelineSyncFollower           = true;
    context.playbackVisualClock.rebase(10.0, 100.0, 1.0, true);
    const MMM::Audio::AudioTimelineClockSnapshot snapshot{
        .positionFrame         = 2000,
        .steadyTimeNanoseconds = 100'000'000'000,
        .sampleRate            = 1000U,
        .playbackRate          = 1.0,
        .state            = MMM::Audio::AudioTimelinePlaybackState::Playing,
        .epoch            = 99U,
        .seekSequence     = 12U,
        .playbackSequence = 12U,
        .sequence         = 99U,
        .valid            = true,
    };
    if ( !MMM::Logic::SessionUtils::applyAudioTimelineTransportSnapshot(
             context,
             "timeline",
             snapshot,
             100.1,
             context.lastConfig.settings.syncConfig) ||
         !near(context.currentTime, 10.1) ||
         !near(context.playbackVisualClock.lastResolvedSteadyTime(), 100.1) ) {
        XERROR("Follower clock was overwritten by the discrete audio block");
        return false;
    }
    return true;
}

/// @brief 验证后台会话不能启动或替换全局音频时间线。
bool testBackgroundSessionCannotControlTransport()
{
    MMM::Logic::SessionContext     context;
    MMM::Logic::PlaybackController controller(context);
    context.isActiveSession = false;
    context.currentTime     = 8.25;

    controller.handleCommand(MMM::Logic::CmdSetPlayState{ true });
    if ( context.isPlaying || !near(context.currentTime, 8.25) ) {
        XERROR("Background session unexpectedly controlled audio transport");
        return false;
    }
    return true;
}

/// @brief 验证手动跳转取消自然结束后的自动回到开头。
bool testSeekCancelsPendingRestart()
{
    MMM::Logic::SessionContext     context;
    MMM::Logic::PlaybackController controller(context);
    context.audioTimelineDescriptor.m_chartEndSeconds = 20.0;
    context.restartPlaybackAfterFinishPending         = true;

    controller.handleCommand(MMM::Logic::CmdSeek{ 8.0 });
    if ( context.restartPlaybackAfterFinishPending ||
         !near(context.currentTime, 8.0) ) {
        XERROR("Manual seek did not cancel pending playback restart");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行复合时间线播放控制权与重播策略测试。
int main()
{
    return testZeroAndMultipleSampleDescriptors() &&
                   testTimelineSwitchUsesCompleteFingerprint() &&
                   testNaturalFinishSnapshotArmsRestart() &&
                   testNonFinishedStopDoesNotArmRestart() &&
                   testFollowerUsesRebasedSourceClock() &&
                   testFinishedTimelineRewindsBeforeActivation() &&
                   testPauseClampsVisualClockToTimelineEnd() &&
                   testBackgroundSessionCannotControlTransport() &&
                   testSeekCancelsPendingRestart()
               ? 0
               : 1;
}
