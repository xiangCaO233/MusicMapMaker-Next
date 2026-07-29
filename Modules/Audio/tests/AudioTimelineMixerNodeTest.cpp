#include "audio/AudioTimelineMixerNode.h"

#include "log/colorful-log.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <ice/config/config.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <memory>
#include <string>
#include <vector>

namespace
{

constexpr float SAMPLE_EPSILON = 1.0e-5F;

/// @brief 记录时间线自然结束通知次数。
void countFinalInputNotification(void* context) noexcept
{
    if ( !context ) return;
    ++(*static_cast<std::size_t*>(context));
}

/// @brief 比较输出缓冲区与两个源区间的加权和。
bool verifyMixedRange(const ice::AudioBuffer& output,
                      const ice::AudioBuffer& firstSource,
                      const ice::AudioBuffer* secondSource,
                      std::size_t secondOutputStart, float firstVolume,
                      float secondVolume)
{
    for ( std::size_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < output.num_frames(); ++frame ) {
            float expected =
                firstSource.raw_ptrs()[channel][frame] * firstVolume;
            if ( secondSource && frame >= secondOutputStart ) {
                expected +=
                    secondSource
                        ->raw_ptrs()[channel][frame - secondOutputStart] *
                    secondVolume;
            }
            if ( std::abs(output.raw_ptrs()[channel][frame] - expected) >
                 SAMPLE_EPSILON ) {
                XERROR(
                    "Timeline mix mismatch: channel={}, frame={}, actual={}, "
                    "expected={}",
                    channel,
                    frame,
                    output.raw_ptrs()[channel][frame],
                    expected);
                return false;
            }
        }
    }
    return true;
}

/// @brief 验证负起点裁切、同文件多声部和 Seek 源帧恢复。
bool testOverlapNegativeStartAndSeek(
    const std::shared_ptr<ice::AudioTrack>&                         track,
    const std::shared_ptr<const MMM::Audio::PreparedTimelineAudio>& audio)
{
    constexpr std::size_t              BLOCK_FRAMES = 32U;
    MMM::Audio::AudioTimelineMixerNode node(
        {
            {
                .eventId    = 20U,
                .sourceKey  = "same",
                .startFrame = 8,
                .volume     = 0.25F,
                .audio      = audio,
            },
            {
                .eventId    = 10U,
                .sourceKey  = "same",
                .startFrame = -16,
                .volume     = 0.5F,
                .audio      = audio,
            },
        },
        96,
        16U);

    if ( node.clipCount() != 2U ||
         node.timelineEndFrame() <
             static_cast<MMM::Audio::AudioTimelineFrame>(track->num_frames()) +
                 8 ) {
        XERROR("Timeline mixer did not preserve both overlapping clips");
        return false;
    }
    node.setMasterGain(0.5F);
    if ( std::abs(node.masterGain() - 0.5F) > SAMPLE_EPSILON ) {
        XERROR("Timeline mixer did not retain its master gain");
        return false;
    }

    ice::AudioBuffer output(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    ice::AudioBuffer firstReference(ice::ICEConfig::internal_format,
                                    BLOCK_FRAMES);
    ice::AudioBuffer secondReference(ice::ICEConfig::internal_format,
                                     BLOCK_FRAMES - 8U);
    firstReference.clear();
    secondReference.clear();
    track->read(firstReference, 16U, BLOCK_FRAMES);
    track->read(secondReference, 0U, BLOCK_FRAMES - 8U);

    node.play();
    node.process(output);
    if ( node.positionFrame() !=
             static_cast<MMM::Audio::AudioTimelineFrame>(BLOCK_FRAMES) ||
         !verifyMixedRange(
             output, firstReference, &secondReference, 8U, 0.25F, 0.125F) ) {
        return false;
    }
    if ( node.leftLevel() <= 0.0F || node.rightLevel() <= 0.0F ) {
        XERROR("Timeline mixer did not publish output levels");
        return false;
    }

    constexpr std::size_t SEEK_BLOCK_FRAMES = 8U;
    node.seek(20);
    ice::AudioBuffer seekOutput(ice::ICEConfig::internal_format,
                                SEEK_BLOCK_FRAMES);
    ice::AudioBuffer seekFirstReference(ice::ICEConfig::internal_format,
                                        SEEK_BLOCK_FRAMES);
    ice::AudioBuffer seekSecondReference(ice::ICEConfig::internal_format,
                                         SEEK_BLOCK_FRAMES);
    seekFirstReference.clear();
    seekSecondReference.clear();
    track->read(seekFirstReference, 36U, SEEK_BLOCK_FRAMES);
    track->read(seekSecondReference, 12U, SEEK_BLOCK_FRAMES);
    node.process(seekOutput);
    if ( !verifyMixedRange(seekOutput,
                           seekFirstReference,
                           &seekSecondReference,
                           0U,
                           0.25F,
                           0.125F) ) {
        XERROR("Timeline seek did not resume every overlapping source frame");
        return false;
    }

    node.pause();
    ice::AudioBuffer pausedOutput(ice::ICEConfig::internal_format,
                                  SEEK_BLOCK_FRAMES);
    node.process(pausedOutput);
    for ( std::size_t channel = 0U; channel < pausedOutput.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < pausedOutput.num_frames();
              ++frame ) {
            if ( pausedOutput.raw_ptrs()[channel][frame] != 0.0F ) {
                XERROR("Paused timeline emitted non-silent audio");
                return false;
            }
        }
    }
    return node.positionFrame() == 28 &&
           node.state() == MMM::Audio::AudioTimelinePlaybackState::Paused &&
           node.leftLevel() == 0.0F && node.rightLevel() == 0.0F;
}

/// @brief 验证半开循环在 R 处截断并于每轮 L 重建交叠 voice。
bool testHalfOpenLoop(const std::shared_ptr<ice::AudioTrack>& track)
{
    const auto audio = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    if ( !audio ) return false;
    MMM::Audio::AudioTimelineMixerNode node(
        {
            {
                .eventId    = 1U,
                .sourceKey  = "loop",
                .startFrame = 4,
                .volume     = 1.0F,
                .audio      = audio,
            },
        },
        32,
        32U);
    if ( !node.setLoop({ 4, 12 }) ) {
        XERROR("Timeline mixer rejected a valid half-open loop");
        return false;
    }
    node.seek(4);
    node.play();

    ice::AudioBuffer output(ice::ICEConfig::internal_format, 16U);
    ice::AudioBuffer reference(ice::ICEConfig::internal_format, 8U);
    reference.clear();
    track->read(reference, 0U, 8U);
    node.process(output);

    for ( std::size_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < output.num_frames(); ++frame ) {
            const float expected = reference.raw_ptrs()[channel][frame % 8U];
            if ( std::abs(output.raw_ptrs()[channel][frame] - expected) >
                 SAMPLE_EPSILON ) {
                XERROR("Half-open timeline loop mismatch at frame {}", frame);
                return false;
            }
        }
    }

    return node.positionFrame() == 4 && node.epoch() >= 3U && !node.finished();
}

/// @brief 验证空资源安全过滤和自然结束状态。
bool testMissingResourceAndFinish()
{
    MMM::Audio::AudioTimelineMixerNode node(
        {
            {
                .eventId    = 1U,
                .sourceKey  = "missing",
                .startFrame = 0,
                .volume     = 1.0F,
                .audio      = {},
            },
        },
        6,
        4U);
    std::size_t finalNotificationCount = 0U;
    node.setFinalInputListener(&finalNotificationCount,
                               &countFinalInputNotification);
    if ( node.clipCount() != 0U ) {
        XERROR("Timeline mixer retained a missing audio resource");
        return false;
    }

    node.play();
    ice::AudioBuffer output(ice::ICEConfig::internal_format, 10U);
    node.process(output);
    if ( node.positionFrame() != 6 || !node.finished() ||
         finalNotificationCount != 1U ||
         node.state() != MMM::Audio::AudioTimelinePlaybackState::Paused ||
         node.requestedState() !=
             MMM::Audio::AudioTimelinePlaybackState::Stopped ) {
        XERROR("Timeline mixer did not stop at the composite end frame");
        return false;
    }
    for ( std::size_t channel = 0U; channel < output.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < output.num_frames(); ++frame ) {
            if ( output.raw_ptrs()[channel][frame] != 0.0F ) return false;
        }
    }

    node.seek(2);
    if ( node.positionFrame() != 2 ) {
        XERROR("Paused timeline did not expose its pending seek target");
        return false;
    }
    ice::AudioBuffer seekOutput(ice::ICEConfig::internal_format, 1U);
    node.process(seekOutput);
    if ( node.positionFrame() != 2 || node.finished() ||
         finalNotificationCount != 1U ||
         node.state() != MMM::Audio::AudioTimelinePlaybackState::Paused ||
         node.requestedState() !=
             MMM::Audio::AudioTimelinePlaybackState::Paused ) {
        return false;
    }

    node.play();
    node.process(output);
    node.process(output);
    return node.finished() && finalNotificationCount == 2U;
}

/// @brief 验证边界查询、精确结束和结束后的调度替换不会自行复活。
bool testInputBoundaryAndFinishedReplacement()
{
    using MMM::Audio::AudioTimelineInputBoundaryKind;
    using MMM::Audio::AudioTimelinePlaybackState;

    MMM::Audio::AudioTimelineMixerNode node({}, 8, 8U);
    std::size_t                        finalNotificationCount = 0U;
    node.setFinalInputListener(&finalNotificationCount,
                               &countFinalInputNotification);
    node.play();

    const auto finalBoundary = node.prepareInputBoundary(16U);
    if ( finalBoundary.frameCount != 8U ||
         finalBoundary.kind != AudioTimelineInputBoundaryKind::Final ) {
        XERROR("Timeline mixer did not expose its exact final input boundary");
        return false;
    }

    ice::AudioBuffer exactOutput(ice::ICEConfig::internal_format, 8U);
    node.process(exactOutput);
    if ( node.positionFrame() != 8 || !node.finished() ||
         finalNotificationCount != 1U ||
         node.requestedState() != AudioTimelinePlaybackState::Stopped ) {
        XERROR("Timeline mixer did not publish an exact natural finish");
        return false;
    }

    node.replaceSchedule({}, 16, 8U);
    ice::AudioBuffer probeOutput(ice::ICEConfig::internal_format, 1U);
    node.process(probeOutput);
    if ( !node.finished() ||
         node.state() != AudioTimelinePlaybackState::Stopped ||
         node.requestedState() != AudioTimelinePlaybackState::Stopped ||
         node.positionFrame() != 0 ) {
        XERROR("Finished timeline restarted after a schedule replacement");
        return false;
    }

    if ( !node.setLoop({ 2, 6 }) ) return false;
    node.process(probeOutput);
    if ( !node.finished() ||
         node.state() != AudioTimelinePlaybackState::Stopped ) {
        XERROR("Loop configuration revived a naturally finished timeline");
        return false;
    }

    node.seek(2);
    node.play();
    const auto loopBoundary = node.prepareInputBoundary(16U);
    if ( loopBoundary.frameCount != 4U ||
         loopBoundary.kind != AudioTimelineInputBoundaryKind::Discontinuity ) {
        XERROR("Timeline mixer did not expose its half-open loop boundary");
        return false;
    }

    ice::AudioBuffer loopOutput(ice::ICEConfig::internal_format, 4U);
    node.process(loopOutput);
    const auto loopSnapshot = node.clockSnapshot();
    return !node.finished() && node.positionFrame() == 2 &&
           node.state() == AudioTimelinePlaybackState::Playing &&
           finalNotificationCount == 1U && loopSnapshot.valid &&
           loopSnapshot.epoch != loopSnapshot.controlEpoch;
}

/// @brief 验证 Stop 与紧随其后的 Play 仍会从零点开启新纪元。
bool testImmediateStopThenPlay()
{
    MMM::Audio::AudioTimelineMixerNode node({}, 32, 8U);
    ice::AudioBuffer block(ice::ICEConfig::internal_format, 4U);
    node.play();
    node.process(block);
    if ( node.positionFrame() != 4 ) return false;

    node.pause();
    node.process(block);
    node.stop();
    node.play();

    ice::AudioBuffer firstRestartFrame(ice::ICEConfig::internal_format, 1U);
    node.process(firstRestartFrame);
    if ( node.positionFrame() != 1 ||
         node.state() != MMM::Audio::AudioTimelinePlaybackState::Playing ) {
        XERROR("Immediate stop-then-play did not restart the timeline at zero");
        return false;
    }
    return true;
}

/// @brief 验证逻辑线程取得的位置、状态、时间戳和控制序列来自一致快照。
bool testCoherentTimelineClockSnapshot()
{
    using MMM::Audio::AudioTimelineMixerNode;
    using MMM::Audio::AudioTimelinePlaybackState;

    AudioTimelineMixerNode node({}, 32, 8U);
    const auto             initial = node.clockSnapshot();
    if ( !initial.valid || (initial.sequence & 1U) != 0U ||
         initial.steadyTimeNanoseconds <= 0 ||
         initial.state != AudioTimelinePlaybackState::Stopped ||
         initial.controlEpoch != initial.epoch ||
         initial.scheduleGeneration == 0U ||
         initial.appliedSeekSequence != initial.seekSequence ||
         initial.appliedPlaybackSequence != initial.playbackSequence ) {
        XERROR("Timeline clock did not publish a coherent initial snapshot");
        return false;
    }

    node.play();
    const auto pendingPlay = node.clockSnapshot();
    if ( !pendingPlay.valid ||
         pendingPlay.state != AudioTimelinePlaybackState::Playing ||
         pendingPlay.playbackSequence == initial.playbackSequence ||
         pendingPlay.appliedPlaybackSequence !=
             initial.appliedPlaybackSequence ) {
        XERROR("Timeline clock did not expose a stable pending Play command");
        return false;
    }

    ice::AudioBuffer block(ice::ICEConfig::internal_format, 4U);
    node.process(block);
    const auto advanced = node.clockSnapshot();
    if ( !advanced.valid || advanced.sequence <= initial.sequence ||
         advanced.positionFrame != 4 ||
         advanced.state != AudioTimelinePlaybackState::Playing ||
         advanced.controlEpoch != advanced.epoch ||
         advanced.scheduleGeneration != initial.scheduleGeneration ||
         advanced.appliedPlaybackSequence != advanced.playbackSequence ) {
        XERROR("Timeline clock position did not advance with its publication");
        return false;
    }

    node.seek(10);
    const auto pendingSeek = node.clockSnapshot();
    if ( !pendingSeek.valid || pendingSeek.positionFrame != 10 ||
         pendingSeek.seekSequence == advanced.seekSequence ||
         pendingSeek.appliedSeekSequence != advanced.appliedSeekSequence ||
         pendingSeek.steadyTimeNanoseconds < advanced.steadyTimeNanoseconds ) {
        XERROR("Timeline clock did not expose a stable pending Seek command");
        return false;
    }

    node.process(block);
    const auto appliedSeek = node.clockSnapshot();
    if ( !appliedSeek.valid || appliedSeek.positionFrame != 14 ||
         appliedSeek.epoch == advanced.epoch ||
         appliedSeek.controlEpoch != appliedSeek.epoch ||
         appliedSeek.appliedSeekSequence != appliedSeek.seekSequence ) {
        XERROR("Timeline clock did not publish the applied Seek epoch");
        return false;
    }

    node.pause();
    const auto pendingPause = node.clockSnapshot();
    if ( !pendingPause.valid ||
         pendingPause.state != AudioTimelinePlaybackState::Paused ) {
        XERROR("Timeline clock did not expose a stable pending Pause command");
        return false;
    }

    AudioTimelineMixerNode finishedNode({}, 4, 4U);
    finishedNode.play();
    finishedNode.process(block);
    const auto finished = finishedNode.clockSnapshot();
    if ( !finished.valid || !finished.finished ) return false;
    finishedNode.play();
    const auto restarted = finishedNode.clockSnapshot();
    if ( !restarted.valid || restarted.positionFrame != 0 ||
         restarted.finished ||
         restarted.state != AudioTimelinePlaybackState::Playing ||
         restarted.appliedPlaybackSequence == restarted.playbackSequence ) {
        XERROR("Finished timeline clock did not expose pending Play at zero");
        return false;
    }

    ice::AudioBuffer restartFrame(ice::ICEConfig::internal_format, 1U);
    finishedNode.process(restartFrame);
    const auto appliedRestart = finishedNode.clockSnapshot();
    if ( !appliedRestart.valid || appliedRestart.positionFrame != 1 ||
         appliedRestart.controlEpoch != appliedRestart.epoch ||
         appliedRestart.appliedPlaybackSequence !=
             appliedRestart.playbackSequence ) {
        XERROR("Finished Play acknowledgement fields were inconsistent");
        return false;
    }
    return true;
}

/// @brief 验证循环规范化、同 block 回绕和调度换代快照字段保持一致。
bool testClockSnapshotLoopAndScheduleGeneration()
{
    using MMM::Audio::AudioTimelineMixerNode;
    using MMM::Audio::AudioTimelinePlaybackState;

    AudioTimelineMixerNode node({}, 64, 8U);
    const auto             initial = node.clockSnapshot();
    if ( !node.setLoop({ 4, 8 }) ) return false;
    node.seek(6);
    node.play();

    ice::AudioBuffer loopBlock(ice::ICEConfig::internal_format, 4U);
    node.process(loopBlock);
    const auto looped = node.clockSnapshot();
    if ( !looped.valid || looped.positionFrame != 6 ||
         looped.state != AudioTimelinePlaybackState::Playing ||
         looped.epoch == looped.controlEpoch ||
         looped.scheduleGeneration != initial.scheduleGeneration ||
         looped.appliedSeekSequence != looped.seekSequence ||
         looped.appliedPlaybackSequence != looped.playbackSequence ) {
        XERROR("Loop block clock fields did not preserve the control boundary");
        return false;
    }

    node.seek(20);
    const auto pendingNormalizedSeek = node.clockSnapshot();
    if ( !pendingNormalizedSeek.valid ||
         pendingNormalizedSeek.positionFrame != 4 ||
         node.positionFrame() != 4 ||
         pendingNormalizedSeek.appliedSeekSequence ==
             pendingNormalizedSeek.seekSequence ) {
        XERROR("Pending looped Seek was not normalized to the loop start");
        return false;
    }

    ice::AudioBuffer afterSeekBlock(ice::ICEConfig::internal_format, 2U);
    node.process(afterSeekBlock);
    const auto appliedNormalizedSeek = node.clockSnapshot();
    if ( !appliedNormalizedSeek.valid ||
         appliedNormalizedSeek.positionFrame != 6 ||
         appliedNormalizedSeek.controlEpoch != appliedNormalizedSeek.epoch ||
         appliedNormalizedSeek.appliedSeekSequence !=
             appliedNormalizedSeek.seekSequence ) {
        XERROR("Normalized Seek acknowledgement fields were inconsistent");
        return false;
    }

    node.clearLoop();
    node.replaceSchedule({}, 64, 8U);
    node.process(afterSeekBlock);
    const auto replacement = node.clockSnapshot();
    if ( !replacement.valid ||
         replacement.scheduleGeneration == looped.scheduleGeneration ||
         replacement.controlEpoch != replacement.epoch ||
         replacement.appliedSeekSequence != replacement.seekSequence ||
         replacement.appliedPlaybackSequence != replacement.playbackSequence ) {
        XERROR("Replacement schedule clock fields were inconsistent");
        return false;
    }
    return true;
}

/// @brief 验证边界查询与紧随拉取共同形成一个不可插入控制命令的区间。
bool testPreparedBoundarySealsNextPull()
{
    MMM::Audio::AudioTimelineMixerNode node({}, 20, 8U);
    node.play();
    const auto prepared = node.prepareInputBoundary(4U);
    if ( prepared.frameCount != 4U ||
         prepared.kind != MMM::Audio::AudioTimelineInputBoundaryKind::None ) {
        return false;
    }

    node.seek(10);
    ice::AudioBuffer firstPull(ice::ICEConfig::internal_format, 4U);
    node.process(firstPull);
    if ( node.blockStartFrame() != 0 || node.positionFrame() != 10 ) {
        XERROR("Control command entered a previously prepared input segment");
        return false;
    }

    const auto afterSeek = node.prepareInputBoundary(4U);
    if ( afterSeek.frameCount != 4U ) return false;
    ice::AudioBuffer secondPull(ice::ICEConfig::internal_format, 4U);
    node.process(secondPull);
    const auto appliedSeek = node.clockSnapshot();
    return node.blockStartFrame() == 10 && node.positionFrame() == 14 &&
           appliedSeek.controlEpoch == appliedSeek.epoch &&
           appliedSeek.appliedSeekSequence == appliedSeek.seekSequence;
}

/// @brief 验证连续调度提交只在 block 起点启用最后一份完整状态。
bool testAtomicScheduleReplacement(
    const std::shared_ptr<ice::AudioTrack>&                         track,
    const std::shared_ptr<const MMM::Audio::PreparedTimelineAudio>& audio)
{
    constexpr std::size_t              BLOCK_FRAMES = 8U;
    MMM::Audio::AudioTimelineMixerNode node(
        {
            {
                .eventId    = 1U,
                .sourceKey  = "old",
                .startFrame = 0,
                .volume     = 0.1F,
                .audio      = audio,
            },
        },
        64,
        BLOCK_FRAMES);
    node.play();

    ice::AudioBuffer discarded(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    node.process(discarded);
    if ( node.positionFrame() !=
         static_cast<MMM::Audio::AudioTimelineFrame>(BLOCK_FRAMES) ) {
        XERROR("Timeline replacement precondition did not advance");
        return false;
    }

    node.replaceSchedule(
        {
            {
                .eventId    = 2U,
                .sourceKey  = "superseded",
                .startFrame = 0,
                .volume     = 0.25F,
                .audio      = audio,
            },
        },
        64,
        BLOCK_FRAMES);
    node.replaceSchedule(
        {
            {
                .eventId    = 3U,
                .sourceKey  = "replacement",
                .startFrame = 0,
                .volume     = 0.75F,
                .audio      = audio,
            },
        },
        64,
        BLOCK_FRAMES);

    ice::AudioBuffer output(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    ice::AudioBuffer reference(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    reference.clear();
    track->read(reference, BLOCK_FRAMES, BLOCK_FRAMES);
    node.process(output);
    if ( !verifyMixedRange(output, reference, nullptr, 0U, 0.75F, 0.0F) ) {
        XERROR("Timeline block observed a partial or superseded schedule");
        return false;
    }
    return node.positionFrame() ==
               static_cast<MMM::Audio::AudioTimelineFrame>(BLOCK_FRAMES * 2U) &&
           node.clipCount() == 1U && !node.finished();
}

/// @brief 验证旧调度只在 block 接管新状态后由控制线程释放。
/// @param track 已完整缓存的测试音轨。
/// @return 验证通过时返回 true。
bool testRetiredScheduleReclamation(
    const std::shared_ptr<ice::AudioTrack>& track)
{
    constexpr std::size_t BLOCK_FRAMES = 8U;
    auto retiredAudio = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    if ( !retiredAudio ) return false;

    std::weak_ptr<const MMM::Audio::PreparedTimelineAudio> retiredAudioWeak =
        retiredAudio;
    MMM::Audio::AudioTimelineMixerNode node(
        {
            {
                .eventId    = 1U,
                .sourceKey  = "retired",
                .startFrame = 0,
                .volume     = 1.0F,
                .audio      = retiredAudio,
            },
        },
        64,
        BLOCK_FRAMES);
    retiredAudio.reset();

    node.replaceSchedule({}, 64, BLOCK_FRAMES);
    if ( retiredAudioWeak.expired() || node.reclaimRetiredSchedules() != 0U ) {
        XERROR("Active schedule was reclaimed before the block boundary");
        return false;
    }

    ice::AudioBuffer output(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    node.process(output);
    if ( retiredAudioWeak.expired() ) {
        XERROR("Audio callback released a retired schedule");
        return false;
    }

    if ( node.reclaimRetiredSchedules() != 1U || !retiredAudioWeak.expired() ) {
        XERROR("Control thread did not release the retired schedule");
        return false;
    }
    return node.reclaimRetiredSchedules() == 0U;
}

}  // namespace

/// @brief 运行实时多采样时间线混音测试。
int main(int argc, char** argv)
{
    if ( argc < 2 ) {
        XERROR("Usage: AudioTimelineMixerNodeTest <sample_path>");
        return 1;
    }

    const std::filesystem::path samplePath(argv[1]);
    ice::ThreadPool             threadPool(2);
    ice::AudioPool              audioPool;
    auto track = audioPool.get_or_load(threadPool, samplePath.string()).lock();
    if ( !track || track->num_frames() < 64U ) {
        XERROR("Failed to prepare timeline mixer test sample");
        return 1;
    }
    const auto audio = MMM::Audio::PreparedTimelineAudio::fromTrack(track);
    if ( !audio ) {
        XERROR("Failed to freeze timeline mixer test PCM");
        return 1;
    }

    const bool passed =
        testOverlapNegativeStartAndSeek(track, audio) &&
        testHalfOpenLoop(track) && testMissingResourceAndFinish() &&
        testInputBoundaryAndFinishedReplacement() &&
        testImmediateStopThenPlay() && testCoherentTimelineClockSnapshot() &&
        testClockSnapshotLoopAndScheduleGeneration() &&
        testPreparedBoundarySealsNextPull() &&
        testAtomicScheduleReplacement(track, audio) &&
        testRetiredScheduleReclamation(track);
    return passed ? 0 : 1;
}
