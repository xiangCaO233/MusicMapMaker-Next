#include "logic/audio/PlaybackVisualClock.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"

#include <cmath>
#include <cstdint>

namespace
{

constexpr double EPSILON = 1.0e-9;

/// @brief 构造可由假壁钟确定性驱动的音频时钟快照。
MMM::Audio::AudioTimelineClockSnapshot makeSnapshot(
    std::int64_t positionFrame, double observationTime,
    MMM::Audio::AudioTimelinePlaybackState state, std::uint64_t sequence,
    std::uint64_t epoch = 1U, std::uint64_t seekSequence = 2U,
    std::uint64_t playbackSequence = 2U, double playbackRate = 1.0)
{
    return {
        .positionFrame         = positionFrame,
        .steadyTimeNanoseconds = static_cast<std::int64_t>(
            std::llround(observationTime * 1'000'000'000.0)),
        .sampleRate              = 1000U,
        .playbackRate            = playbackRate,
        .state                   = state,
        .epoch                   = epoch,
        .controlEpoch            = epoch,
        .scheduleGeneration      = 1U,
        .seekSequence            = seekSequence,
        .appliedSeekSequence     = seekSequence,
        .playbackSequence        = playbackSequence,
        .appliedPlaybackSequence = playbackSequence,
        .sequence                = sequence,
        .finished                = false,
        .valid                   = true,
    };
}

/// @brief 比较确定性时钟结果。
bool near(double lhs, double rhs, double epsilon = EPSILON)
{
    return std::abs(lhs - rhs) <= epsilon;
}

/// @brief 验证同一音频 block 内多次逻辑更新保持连续匀速。
bool testSameBlockContinuousAdvance()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 10.0;

    const auto block = makeSnapshot(
        1000, 10.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    const double first  = clock.update(block, 10.0, config);
    const double second = clock.update(block, 10.01, config);
    const double third  = clock.update(block, 10.02, config);
    if ( !near(first, 1.0) || !near(second, 1.01) || !near(third, 1.02) ) {
        XERROR("Visual clock repeated a discrete audio block position");
        return false;
    }

    auto nextBlock                  = block;
    nextBlock.positionFrame         = 1020;
    nextBlock.steadyTimeNanoseconds = 10'020'000'000;
    nextBlock.sequence              = 4U;
    const double next               = clock.update(nextBlock, 10.021, config);
    if ( next < third || !near(next, 1.021) ) {
        XERROR("A new audio block moved the visual clock backwards");
        return false;
    }
    return true;
}

/// @brief 验证同步周期到期前的新 block 误差不会逐帧拉回视觉时间。
bool testNewBlockDoesNotCalibrateBeforeSyncInterval()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 1.0;

    const auto initial = makeSnapshot(
        0, 100.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 100.0, config));

    auto delayed                  = initial;
    delayed.positionFrame         = 100;
    delayed.steadyTimeNanoseconds = 100'200'000'000;
    delayed.sequence              = 4U;
    if ( !near(clock.update(delayed, 100.2, config), 0.2) ||
         !near(clock.update(delayed, 100.8, config), 0.8) ) {
        XERROR("New audio block calibrated before syncInterval elapsed");
        return false;
    }
    return true;
}

/// @brief 验证暂停冻结以及 Seek/循环纪元允许显式回退。
bool testPauseAndDiscontinuities()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    const auto                      playing = makeSnapshot(
        1000, 20.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(playing, 20.0, config));
    static_cast<void>(clock.update(playing, 20.1, config));

    auto paused             = playing;
    paused.positionFrame    = 1100;
    paused.state            = MMM::Audio::AudioTimelinePlaybackState::Paused;
    paused.playbackSequence = 4U;
    paused.steadyTimeNanoseconds = 20'100'000'000;
    const double frozen          = clock.update(paused, 20.12, config);
    if ( !near(frozen, 1.1) ||
         !near(clock.update(paused, 21.0, config), frozen) ) {
        XERROR("Pause did not freeze the continuous visual clock");
        return false;
    }

    auto seek                  = paused;
    seek.positionFrame         = 500;
    seek.epoch                 = 2U;
    seek.controlEpoch          = 2U;
    seek.seekSequence          = 4U;
    seek.sequence              = 4U;
    seek.steadyTimeNanoseconds = 21'000'000'000;
    if ( !near(clock.update(seek, 21.0, config), 0.5) ) {
        XERROR("Seek epoch did not re-anchor to its explicit target");
        return false;
    }

    auto loop             = seek;
    loop.positionFrame    = 250;
    loop.state            = MMM::Audio::AudioTimelinePlaybackState::Playing;
    loop.epoch            = 3U;
    loop.controlEpoch     = 3U;
    loop.playbackSequence = 6U;
    loop.sequence         = 6U;
    loop.steadyTimeNanoseconds = 21'100'000'000;
    if ( !near(clock.update(loop, 21.1, config), 0.25) ) {
        XERROR("Loop epoch did not allow an intentional backward jump");
        return false;
    }
    return true;
}

/// @brief 验证已观察的 pending Seek 被音频线程确认后不会前跳一个 block。
bool testPendingSeekAppliedAckKeepsContinuousTime()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 10.0;

    const auto initial = makeSnapshot(
        1000, 10.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 10.0, config));

    auto pending                  = initial;
    pending.positionFrame         = 5000;
    pending.steadyTimeNanoseconds = 10'100'000'000;
    pending.seekSequence          = 4U;
    pending.appliedSeekSequence   = 2U;
    if ( !near(clock.update(pending, 10.1, config), 5.0) ) {
        XERROR("Pending Seek did not expose its target immediately");
        return false;
    }

    auto applied                = pending;
    applied.positionFrame       = 5020;
    applied.epoch               = 2U;
    applied.controlEpoch        = 2U;
    applied.appliedSeekSequence = 4U;
    applied.sequence            = 4U;
    if ( !near(clock.update(applied, 10.1, config), 5.0) ) {
        XERROR("Applied Seek acknowledgement advanced by one audio block");
        return false;
    }
    return true;
}

/// @brief 验证 Seek 应用后同一 block 的循环回绕仍按最终位置重建锚点。
bool testSeekAckWithSameBlockLoopStillReanchors()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 10.0;

    const auto initial = makeSnapshot(
        1000, 20.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 20.0, config));

    auto pending                  = initial;
    pending.positionFrame         = 10000;
    pending.steadyTimeNanoseconds = 20'100'000'000;
    pending.seekSequence          = 4U;
    pending.appliedSeekSequence   = 2U;
    if ( !near(clock.update(pending, 20.1, config), 10.0) ) {
        return false;
    }

    auto looped                  = pending;
    looped.positionFrame         = 6000;
    looped.steadyTimeNanoseconds = 20'100'000'000;
    looped.epoch                 = 4U;
    looped.controlEpoch          = 3U;
    looped.appliedSeekSequence   = 4U;
    looped.sequence              = 4U;
    if ( !near(clock.update(looped, 20.1, config), 6.0) ) {
        XERROR("Same-block loop was mistaken for a pure control ack");
        return false;
    }
    return true;
}

/// @brief 验证自然结束后的 pending Play 在 Seek 归零确认时不二次前跳。
bool testFinishedPlayAckKeepsPendingRestartPosition()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 10.0;

    auto finished = makeSnapshot(
        12000, 30.0, MMM::Audio::AudioTimelinePlaybackState::Stopped, 2U);
    finished.epoch        = 5U;
    finished.controlEpoch = 5U;
    finished.finished     = true;
    static_cast<void>(clock.update(finished, 30.0, config));

    auto pending                  = finished;
    pending.positionFrame         = 0;
    pending.steadyTimeNanoseconds = 30'100'000'000;
    pending.state            = MMM::Audio::AudioTimelinePlaybackState::Playing;
    pending.playbackSequence = 4U;
    pending.appliedPlaybackSequence = 2U;
    pending.finished                = false;
    if ( !near(clock.update(pending, 30.1, config), 0.0) ) {
        return false;
    }

    auto applied                    = pending;
    applied.positionFrame           = 20;
    applied.epoch                   = 6U;
    applied.controlEpoch            = 6U;
    applied.appliedPlaybackSequence = 4U;
    applied.sequence                = 4U;
    if ( !near(clock.update(applied, 30.1, config), 0.0) ) {
        XERROR("Finished Play acknowledgement advanced by one audio block");
        return false;
    }
    return true;
}

/// @brief 验证 Stop 的 Seek 与播放命令双确认只消费一次离散控制。
bool testStopDoubleAckKeepsPendingStopPosition()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 10.0;

    const auto initial = makeSnapshot(
        1000, 40.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 40.0, config));

    auto pending                  = initial;
    pending.positionFrame         = 0;
    pending.steadyTimeNanoseconds = 40'100'000'000;
    pending.state        = MMM::Audio::AudioTimelinePlaybackState::Stopped;
    pending.seekSequence = 4U;
    pending.appliedSeekSequence     = 2U;
    pending.playbackSequence        = 4U;
    pending.appliedPlaybackSequence = 2U;
    if ( !near(clock.update(pending, 40.1, config), 0.0) ) {
        return false;
    }

    auto applied                    = pending;
    applied.epoch                   = 3U;
    applied.controlEpoch            = 3U;
    applied.appliedSeekSequence     = 4U;
    applied.appliedPlaybackSequence = 4U;
    applied.sequence                = 4U;
    if ( !near(clock.update(applied, 40.1, config), 0.0) ) {
        XERROR("Stop double acknowledgement changed the pending stop anchor");
        return false;
    }
    return true;
}

/// @brief 验证调度换代不比较跨代 epoch，且明显位置变化仍会重建锚点。
bool testScheduleGenerationTransitionRules()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 10.0;

    auto initial = makeSnapshot(
        10000, 50.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    initial.epoch        = 100U;
    initial.controlEpoch = 100U;
    static_cast<void>(clock.update(initial, 50.0, config));

    auto pending                  = initial;
    pending.positionFrame         = 10100;
    pending.steadyTimeNanoseconds = 50'100'000'000;
    pending.seekSequence          = 4U;
    pending.appliedSeekSequence   = 2U;
    if ( !near(clock.update(pending, 50.1, config), 10.1) ) {
        return false;
    }

    auto replacement                  = pending;
    replacement.positionFrame         = 10400;
    replacement.steadyTimeNanoseconds = 50'200'000'000;
    replacement.epoch                 = 1U;
    replacement.controlEpoch          = 1U;
    replacement.scheduleGeneration    = 2U;
    replacement.appliedSeekSequence   = 4U;
    replacement.sequence              = 4U;
    if ( !near(clock.update(replacement, 50.2, config), 10.2) ) {
        XERROR("Schedule generation compared unrelated transport epochs");
        return false;
    }

    auto displaced                  = replacement;
    displaced.positionFrame         = 25000;
    displaced.steadyTimeNanoseconds = 50'300'000'000;
    displaced.epoch                 = 0U;
    displaced.controlEpoch          = 0U;
    displaced.scheduleGeneration    = 3U;
    displaced.sequence              = 6U;
    if ( !near(clock.update(displaced, 50.3, config), 25.0) ) {
        XERROR("Displaced replacement schedule did not re-anchor");
        return false;
    }
    return true;
}

/// @brief 验证调度换代与旧控制确认不能吞掉同帧新出现的 pending Seek。
bool testNewPendingSeekOverridesScheduleAck()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 10.0;

    const auto initial = makeSnapshot(
        1000, 60.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 60.0, config));

    auto pendingPlayback                    = initial;
    pendingPlayback.positionFrame           = 1100;
    pendingPlayback.steadyTimeNanoseconds   = 60'100'000'000;
    pendingPlayback.playbackSequence        = 4U;
    pendingPlayback.appliedPlaybackSequence = 2U;
    if ( !near(clock.update(pendingPlayback, 60.1, config), 1.1) ) {
        return false;
    }

    auto replacement                    = pendingPlayback;
    replacement.positionFrame           = 1400;
    replacement.steadyTimeNanoseconds   = 60'200'000'000;
    replacement.scheduleGeneration      = 2U;
    replacement.seekSequence            = 4U;
    replacement.appliedSeekSequence     = 2U;
    replacement.appliedPlaybackSequence = 4U;
    replacement.sequence                = 4U;
    if ( !near(clock.update(replacement, 60.2, config), 1.4) ) {
        XERROR("Schedule acknowledgement swallowed a new pending Seek");
        return false;
    }
    return true;
}

/// @brief 验证长暂停恢复后不会沿用暂停前的音频时间原点。
bool testLongPauseResumeResetsAudioOrigin()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 1.0;

    const auto playing = makeSnapshot(
        1000, 70.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(playing, 70.0, config));

    auto paused                  = playing;
    paused.positionFrame         = 1100;
    paused.steadyTimeNanoseconds = 70'100'000'000;
    paused.state            = MMM::Audio::AudioTimelinePlaybackState::Paused;
    paused.playbackSequence = 4U;
    paused.sequence         = 4U;
    if ( !near(clock.update(paused, 70.1, config), 1.1) ||
         !near(clock.update(paused, 75.1, config), 1.1) ) {
        return false;
    }

    auto resumed             = paused;
    resumed.state            = MMM::Audio::AudioTimelinePlaybackState::Playing;
    resumed.playbackSequence = 6U;
    resumed.steadyTimeNanoseconds = 75'100'000'000;
    if ( !near(clock.update(resumed, 75.1, config), 1.1) ) {
        return false;
    }

    auto firstResumedBlock                  = resumed;
    firstResumedBlock.positionFrame         = 1150;
    firstResumedBlock.steadyTimeNanoseconds = 75'200'000'000;
    firstResumedBlock.sequence              = 6U;
    if ( !near(clock.update(firstResumedBlock, 75.2, config), 1.2) ) {
        XERROR("Resume reused the pre-pause audio origin and hard re-anchored");
        return false;
    }
    return true;
}

/// @brief 验证半速与双速均按谱面时间倍率连续推进。
bool testPlaybackRates()
{
    MMM::Config::SyncConfig config;
    for ( const auto [rate, expected] :
          { std::pair{ 0.5, 2.1 }, std::pair{ 2.0, 2.4 } } ) {
        MMM::Logic::PlaybackVisualClock clock;
        const auto                      snapshot =
            makeSnapshot(2000,
                         30.0,
                         MMM::Audio::AudioTimelinePlaybackState::Playing,
                         2U,
                         1U,
                         2U,
                         2U,
                         rate);
        static_cast<void>(clock.update(snapshot, 30.0, config));
        if ( !near(clock.update(snapshot, 30.2, config), expected) ) {
            XERROR("Visual clock did not apply playback rate {}", rate);
            return false;
        }
    }
    return true;
}

/// @brief 验证播放中切换倍率只重建斜率，不改变切换瞬间的位置。
bool testPlaybackRateRebase()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    auto                            snapshot = makeSnapshot(
        1000, 50.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(snapshot, 50.0, config));
    const double beforeChange = clock.update(snapshot, 50.2, config);

    snapshot.playbackRate          = 2.0;
    const double atChange          = clock.update(snapshot, 50.2, config);
    const double afterChange       = clock.update(snapshot, 50.3, config);
    snapshot.positionFrame         = 1400;
    snapshot.steadyTimeNanoseconds = 50'300'000'000;
    snapshot.sequence              = 4U;
    const double firstNewBlock     = clock.update(snapshot, 50.3, config);
    if ( !near(beforeChange, 1.2) || !near(atChange, beforeChange) ||
         !near(afterChange, 1.4) || !near(firstNewBlock, 1.4) ) {
        XERROR("Playback speed change did not preserve and rebase visual time");
        return false;
    }
    return true;
}

/// @brief 验证替换时间线后即使命令序列重号，也不会沿用旧节点锚点。
bool testReplacementWithReusedSequences()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    const auto                      oldNode = makeSnapshot(
        10000, 60.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(oldNode, 60.0, config));
    static_cast<void>(clock.update(oldNode, 60.2, config));

    clock.reset();
    clock.rebase(3.0, 60.2, 1.0, true);
    const auto replacementNode = makeSnapshot(
        3000, 60.2, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    const double replaced = clock.update(replacementNode, 60.2, config);
    if ( !near(replaced, 3.0) ) {
        XERROR("Replacement timeline reused the previous node clock anchor");
        return false;
    }
    return true;
}

/// @brief 验证周期校准沿用历史低通、死区和积分修正语义。
bool testHistoricalIntegralCalibration()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval    = 1.0;
    config.integralFactor  = 0.1F;
    config.mode            = MMM::Config::SyncMode::WaterTank;
    config.waterTankBuffer = 10.0F;

    const auto initial = makeSnapshot(
        0, 100.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 100.0, config));

    auto delayed                  = initial;
    delayed.positionFrame         = 1000;
    delayed.steadyTimeNanoseconds = 101'100'000'000;
    delayed.sequence              = 4U;
    const double corrected        = clock.update(delayed, 101.1, config);
    if ( !near(corrected, 1.0995, 1.0e-8) ) {
        XERROR(
            "Visual clock integral calibration drifted from legacy semantics");
        return false;
    }
    return true;
}

/// @brief 验证大误差校准使用低通音频估计而非原始 block 位置。
bool testLargeErrorUsesSmoothedAudioEstimate()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval = 1.0;

    const auto initial = makeSnapshot(
        0, 100.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 100.0, config));

    auto delayed                  = initial;
    delayed.positionFrame         = 20000;
    delayed.steadyTimeNanoseconds = 102'000'000'000;
    delayed.sequence              = 4U;
    const double corrected        = clock.update(delayed, 102.0, config);
    if ( !near(corrected, 2.9, 1.0e-8) ) {
        XERROR(
            "Large visual clock correction ignored the smoothed audio "
            "estimate");
        return false;
    }
    return true;
}

/// @brief 验证积分因子保留历史超调语义而不被限制到单位区间。
bool testIntegralFactorIsNotClamped()
{
    MMM::Logic::PlaybackVisualClock clock;
    MMM::Config::SyncConfig         config;
    config.syncInterval   = 1.0;
    config.integralFactor = 2.0F;

    const auto initial = makeSnapshot(
        0, 100.0, MMM::Audio::AudioTimelinePlaybackState::Playing, 2U);
    static_cast<void>(clock.update(initial, 100.0, config));

    auto delayed                  = initial;
    delayed.positionFrame         = 1000;
    delayed.steadyTimeNanoseconds = 101'100'000'000;
    delayed.sequence              = 4U;
    const double corrected        = clock.update(delayed, 101.1, config);
    if ( !near(corrected, 1.09, 1.0e-8) ) {
        XERROR("Visual clock integral factor was clamped");
        return false;
    }
    return true;
}

/// @brief 验证状态栏、波形、频谱及三类画布共用同一快照补间比例。
bool testSharedRenderSnapshotResolution()
{
    MMM::Logic::RenderSnapshot snapshot;
    snapshot.isPlaying                    = true;
    snapshot.currentTime                  = 5.0;
    snapshot.playbackTime                 = 4.0;
    snapshot.totalTime                    = 20.0;
    snapshot.snapshotSysTime              = 40.0;
    snapshot.playbackSpeed                = 2.0;
    snapshot.allowUiPlaybackInterpolation = true;
    snapshot.uiInterpolationAbsYSpeed     = 100.0;
    snapshot.uiInterpolationYOffsetScale  = 1.5;
    snapshot.renderScaleY                 = 2.0F;

    const double now            = 40.025;
    const double elapsed        = snapshot.playbackInterpolationElapsed(now);
    const double basicOffset    = snapshot.getInterpolatedOffset(elapsed);
    const double timelineOffset = snapshot.getInterpolatedOffset(elapsed);
    const double previewOffset =
        snapshot.getInterpolatedOffset(elapsed) * snapshot.renderScaleY;
    if ( !near(snapshot.resolveCurrentTimeAt(now), 5.05) ||
         !near(snapshot.resolvePlaybackTimeAt(now), 4.05) ||
         !near(basicOffset, 3.75) || !near(timelineOffset, 3.75) ||
         !near(previewOffset, 7.5) ) {
        XERROR(
            "Shared render snapshot time or canvas offset proportions differ");
        return false;
    }

    snapshot.playbackTime = snapshot.totalTime = 10.0;
    snapshot.currentTime                       = 11.0;
    snapshot.snapshotSysTime                   = 41.0;
    if ( !near(snapshot.playbackInterpolationElapsed(41.025), 0.0) ||
         !near(snapshot.resolveCurrentTimeAt(41.025), 11.0) ||
         !near(snapshot.resolvePlaybackTimeAt(41.025), 10.0) ) {
        XERROR("Final stretcher tail continued UI playback interpolation");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行连续播放视觉时钟与共享 UI 补间测试。
int main()
{
    return testSameBlockContinuousAdvance() &&
                   testNewBlockDoesNotCalibrateBeforeSyncInterval() &&
                   testPauseAndDiscontinuities() &&
                   testPendingSeekAppliedAckKeepsContinuousTime() &&
                   testSeekAckWithSameBlockLoopStillReanchors() &&
                   testFinishedPlayAckKeepsPendingRestartPosition() &&
                   testStopDoubleAckKeepsPendingStopPosition() &&
                   testScheduleGenerationTransitionRules() &&
                   testNewPendingSeekOverridesScheduleAck() &&
                   testLongPauseResumeResetsAudioOrigin() &&
                   testPlaybackRates() && testPlaybackRateRebase() &&
                   testReplacementWithReusedSequences() &&
                   testHistoricalIntegralCalibration() &&
                   testLargeErrorUsesSmoothedAudioEstimate() &&
                   testIntegralFactorIsNotClamped() &&
                   testSharedRenderSnapshotResolution()
               ? 0
               : 1;
}
