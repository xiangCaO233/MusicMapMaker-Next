#include "audio/AudioTimelineMixerNode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ice/manage/AudioTrack.hpp>
#include <limits>
#include <ranges>
#include <span>
#include <utility>

namespace MMM::Audio
{
namespace
{

/// @brief 将 size_t 帧数限制为有符号时间线可表达范围。
[[nodiscard]] AudioTimelineFrame frameCountFromSize(std::size_t value) noexcept
{
    constexpr auto MAX_FRAME = static_cast<std::uint64_t>(
        std::numeric_limits<AudioTimelineFrame>::max());
    return static_cast<AudioTimelineFrame>(
        std::min(static_cast<std::uint64_t>(value), MAX_FRAME));
}

/// @brief 对时间线帧执行上界饱和加法。
[[nodiscard]] AudioTimelineFrame saturatingFrameAdd(
    AudioTimelineFrame frame, AudioTimelineFrame positiveDelta) noexcept
{
    if ( positiveDelta <= 0 ) return frame;
    constexpr AudioTimelineFrame MAX_FRAME =
        std::numeric_limits<AudioTimelineFrame>::max();
    if ( frame > MAX_FRAME - positiveDelta ) return MAX_FRAME;
    return frame + positiveDelta;
}

/// @brief 在不触发有符号溢出的前提下计算到排除边界的距离。
[[nodiscard]] AudioTimelineFrame framesUntil(
    AudioTimelineFrame position, AudioTimelineFrame boundary) noexcept
{
    if ( position >= boundary ) return 0;
    const auto     distance  = static_cast<std::uint64_t>(boundary) -
                               static_cast<std::uint64_t>(position);
    constexpr auto MAX_FRAME = static_cast<std::uint64_t>(
        std::numeric_limits<AudioTimelineFrame>::max());
    return static_cast<AudioTimelineFrame>(std::min(distance, MAX_FRAME));
}

/// @brief 判断序列锁版本是否表示一次完成且尚未应用的写入。
[[nodiscard]] bool hasStableUpdate(std::uint64_t sequence,
                                   std::uint64_t appliedSequence) noexcept
{
    return sequence != appliedSequence && (sequence & 1U) == 0U;
}

}  // namespace

std::shared_ptr<const PreparedTimelineAudio> PreparedTimelineAudio::fromTrack(
    std::shared_ptr<ice::AudioTrack> track)
{
    if ( !track ) return {};
    const std::size_t frameCount = track->num_frames();
    if ( frameCount == 0U ) return {};

    std::vector<std::span<const float>> channelViews;
    static_cast<void>(track->origin(channelViews, 0.0, frameCount));
    if ( channelViews.empty() ) return {};

    const std::size_t availableFrames = std::ranges::min(
        channelViews |
        std::views::transform([](const std::span<const float> channel) {
            return channel.size();
        }));
    if ( availableFrames == 0U ) return {};
    for ( auto& channel : channelViews ) {
        channel = channel.first(availableFrames);
    }
    return std::make_shared<const PreparedTimelineAudio>(
        std::move(track), std::move(channelViews));
}

std::shared_ptr<const PreparedTimelineAudio>
PreparedTimelineAudio::fromOwnedChannels(
    std::vector<std::vector<float>>  channels,
    std::shared_ptr<ice::AudioTrack> sourceOwner)
{
    if ( channels.empty() ) return {};
    const auto frameCount = std::ranges::min(
        channels | std::views::transform([](const std::vector<float>& channel) {
            return channel.size();
        }));
    if ( frameCount == 0U ) return {};
    for ( auto& channel : channels ) {
        channel.resize(frameCount);
    }
    return std::make_shared<const PreparedTimelineAudio>(
        std::move(channels), std::move(sourceOwner));
}

PreparedTimelineAudio::PreparedTimelineAudio(
    std::shared_ptr<ice::AudioTrack>    track,
    std::vector<std::span<const float>> channelViews)
    : m_sourceOwner(std::move(track))
    , m_channelViews(std::move(channelViews))
    , m_frameCount(m_channelViews.empty() ? 0U : m_channelViews.front().size())
{
}

PreparedTimelineAudio::PreparedTimelineAudio(
    std::vector<std::vector<float>>  channels,
    std::shared_ptr<ice::AudioTrack> sourceOwner)
    : m_sourceOwner(std::move(sourceOwner))
    , m_ownedChannels(std::move(channels))
    , m_frameCount(m_ownedChannels.empty() ? 0U
                                           : m_ownedChannels.front().size())
{
    m_channelViews.reserve(m_ownedChannels.size());
    for ( const auto& channel : m_ownedChannels ) {
        m_channelViews.emplace_back(channel);
    }
}

std::size_t PreparedTimelineAudio::numFrames() const noexcept
{
    return m_frameCount;
}

std::size_t PreparedTimelineAudio::numChannels() const noexcept
{
    return m_channelViews.size();
}

std::span<const float> PreparedTimelineAudio::channel(
    std::size_t channelIndex) const noexcept
{
    if ( channelIndex >= m_channelViews.size() ) return {};
    return m_channelViews[channelIndex];
}

std::size_t PreparedTimelineAudio::read(ice::AudioBuffer& buffer,
                                        std::size_t       startFrame,
                                        std::size_t frameCount) const noexcept
{
    if ( startFrame >= m_frameCount || frameCount == 0U ||
         buffer.raw_ptrs() == nullptr || m_channelViews.empty() ) {
        return 0U;
    }

    const std::size_t framesToCopy = std::min(
        { frameCount, m_frameCount - startFrame, buffer.num_frames() });
    if ( framesToCopy == 0U ) return 0U;

    const std::size_t outputChannels = buffer.num_channels();
    if ( m_channelViews.size() == 1U && outputChannels > 1U ) {
        const float* source = m_channelViews.front().data() + startFrame;
        for ( std::size_t channel = 0U; channel < outputChannels; ++channel ) {
            std::memcpy(buffer.raw_ptrs()[channel],
                        source,
                        framesToCopy * sizeof(float));
        }
        return framesToCopy;
    }

    const std::size_t channelsToCopy =
        std::min(outputChannels, m_channelViews.size());
    for ( std::size_t channel = 0U; channel < channelsToCopy; ++channel ) {
        std::memcpy(buffer.raw_ptrs()[channel],
                    m_channelViews[channel].data() + startFrame,
                    framesToCopy * sizeof(float));
    }
    return framesToCopy;
}

AudioTimelineMixerNode::AudioTimelineMixerNode(
    std::vector<PreparedTimelineClip> clips,
    AudioTimelineFrame                requestedTimelineEndFrame,
    std::size_t                       maximumProcessFrames)
{
    static_assert(std::atomic<ScheduleState*>::is_always_lock_free);
    auto initialState =
        std::make_unique<ScheduleState>(prepareClips(std::move(clips)),
                                        requestedTimelineEndFrame,
                                        maximumProcessFrames);
    m_publishedTimelineEndFrame.store(initialState->timelineEndFrame,
                                      std::memory_order_relaxed);
    m_publishedClipCount.store(initialState->clips.size(),
                               std::memory_order_relaxed);
    m_scheduleState = initialState.release();
    publishTransportSnapshot();
}

AudioTimelineMixerNode::~AudioTimelineMixerNode()
{
    std::unique_ptr<ScheduleState> pending(
        m_pendingSchedule.exchange(nullptr, std::memory_order_acquire));
    std::unique_ptr<ScheduleState> active(m_scheduleState);
    m_scheduleState = nullptr;
    static_cast<void>(reclaimRetiredSchedules());
}

AudioTimelineMixerNode::ScheduleState::ScheduleState(
    std::vector<PreparedTimelineClip> preparedClips,
    AudioTimelineFrame                requestedTimelineEndFrame,
    std::size_t                       requestedMaximumProcessFrames)
    : clips(std::move(preparedClips))
    , transport(AudioTimelineMixerNode::buildClipSpecs(clips))
    , timelineEndFrame(AudioTimelineMixerNode::calculateTimelineEndFrame(
          clips, requestedTimelineEndFrame))
    , maximumProcessFrames(
          std::max<std::size_t>(requestedMaximumProcessFrames, 1U))
    , sourceScratch(ice::ICEConfig::internal_format, maximumProcessFrames)
    , activeSpanScratch(clips.size())
{
}

void AudioTimelineMixerNode::replaceSchedule(
    std::vector<PreparedTimelineClip> clips,
    AudioTimelineFrame                requestedTimelineEndFrame,
    std::size_t                       maximumProcessFrames)
{
    static_cast<void>(reclaimRetiredSchedules());
    auto replacement =
        std::make_unique<ScheduleState>(prepareClips(std::move(clips)),
                                        requestedTimelineEndFrame,
                                        maximumProcessFrames);
    m_publishedTimelineEndFrame.store(replacement->timelineEndFrame,
                                      std::memory_order_relaxed);
    m_publishedClipCount.store(replacement->clips.size(),
                               std::memory_order_relaxed);

    ScheduleState*                 rawReplacement = replacement.release();
    std::unique_ptr<ScheduleState> superseded(
        m_pendingSchedule.exchange(rawReplacement, std::memory_order_acq_rel));
}

std::size_t AudioTimelineMixerNode::reclaimRetiredSchedules() noexcept
{
    if ( m_retiredSchedules.load(std::memory_order_acquire) == nullptr ) {
        return 0U;
    }
    ScheduleState* retired =
        m_retiredSchedules.exchange(nullptr, std::memory_order_acquire);
    std::size_t reclaimedCount = 0U;
    while ( retired ) {
        ScheduleState*                 next = retired->nextRetired;
        std::unique_ptr<ScheduleState> owner(retired);
        retired = next;
        ++reclaimedCount;
    }
    return reclaimedCount;
}

void AudioTimelineMixerNode::setFinalInputListener(
    void* context, FinalInputListener listener) noexcept
{
    m_finalInputListenerContext = listener ? context : nullptr;
    m_finalInputListener        = listener;
}

void AudioTimelineMixerNode::play() noexcept
{
    m_publishedFinished.store(false, std::memory_order_relaxed);
    requestPlaybackCommand(PlaybackCommand::Play);
}

void AudioTimelineMixerNode::pause() noexcept
{
    requestPlaybackCommand(PlaybackCommand::Pause);
}

void AudioTimelineMixerNode::stop() noexcept
{
    m_seekSequence.fetch_add(1U, std::memory_order_acq_rel);
    m_requestedSeekFrame.store(0, std::memory_order_relaxed);
    m_seekSequence.fetch_add(1U, std::memory_order_release);
    requestPlaybackCommand(PlaybackCommand::Stop);
}

void AudioTimelineMixerNode::seek(AudioTimelineFrame frame) noexcept
{
    if ( m_publishedFinished.exchange(false, std::memory_order_relaxed) ) {
        requestPlaybackCommand(PlaybackCommand::Pause);
    }
    m_seekSequence.fetch_add(1U, std::memory_order_acq_rel);
    m_requestedSeekFrame.store(frame, std::memory_order_relaxed);
    m_seekSequence.fetch_add(1U, std::memory_order_release);
}

bool AudioTimelineMixerNode::setLoop(AudioTimelineLoopRange range) noexcept
{
    if ( range.startFrame >= range.endFrame ) return false;

    m_loopSequence.fetch_add(1U, std::memory_order_acq_rel);
    m_requestedLoopStartFrame.store(range.startFrame,
                                    std::memory_order_relaxed);
    m_requestedLoopEndFrame.store(range.endFrame, std::memory_order_relaxed);
    m_requestedLoopEnabled.store(true, std::memory_order_relaxed);
    m_loopSequence.fetch_add(1U, std::memory_order_release);
    return true;
}

void AudioTimelineMixerNode::clearLoop() noexcept
{
    m_loopSequence.fetch_add(1U, std::memory_order_acq_rel);
    m_requestedLoopEnabled.store(false, std::memory_order_relaxed);
    m_loopSequence.fetch_add(1U, std::memory_order_release);
}

AudioTimelineFrame AudioTimelineMixerNode::positionFrame() const noexcept
{
    const auto seekSequence = m_seekSequence.load(std::memory_order_acquire);
    if ( (seekSequence & 1U) == 0U &&
         seekSequence !=
             m_publishedAppliedSeekSequence.load(std::memory_order_acquire) ) {
        const auto requestedFrame =
            m_requestedSeekFrame.load(std::memory_order_relaxed);
        if ( m_seekSequence.load(std::memory_order_acquire) == seekSequence ) {
            return requestedFrame;
        }
    }
    if ( m_requestedPlaybackCommand.load(std::memory_order_relaxed) ==
         PlaybackCommand::Stop ) {
        return 0;
    }
    return m_publishedPositionFrame.load(std::memory_order_relaxed);
}

AudioTimelineFrame AudioTimelineMixerNode::blockStartFrame() const noexcept
{
    return m_publishedBlockStartFrame.load(std::memory_order_relaxed);
}

AudioTimelineInputBoundary AudioTimelineMixerNode::prepareInputBoundary(
    std::size_t maximumFrames) noexcept
{
    m_hasPreparedInputBoundary = false;
    m_preparedInputFrameCount  = 0U;
    applyPendingSchedule();
    applyPendingControls();
    if ( !m_scheduleState || maximumFrames == 0U ||
         m_scheduleState->transport.state() !=
             AudioTimelinePlaybackState::Playing ) {
        publishTransportSnapshot();
        return {};
    }

    auto& schedule = *m_scheduleState;
    while ( true ) {
        const auto loopRange = schedule.transport.loopRange();
        const auto position  = schedule.transport.positionFrame();
        if ( loopRange && position >= loopRange->endFrame ) {
            schedule.transport.seek(loopRange->startFrame);
            continue;
        }
        if ( !loopRange && position >= schedule.timelineEndFrame ) {
            schedule.transport.pause();
            markFinishedAndNotify();
            publishTransportSnapshot();
            return {
                .frameCount = 0U,
                .kind       = AudioTimelineInputBoundaryKind::Final,
            };
        }

        const auto boundary =
            loopRange ? loopRange->endFrame : schedule.timelineEndFrame;
        const auto distance = framesUntil(position, boundary);
        const auto frameCount =
            std::min(frameCountFromSize(maximumFrames), distance);
        AudioTimelineInputBoundary result{
            .frameCount = static_cast<std::size_t>(frameCount),
            .kind =
                frameCount == distance
                    ? (loopRange ? AudioTimelineInputBoundaryKind::Discontinuity
                                 : AudioTimelineInputBoundaryKind::Final)
                    : AudioTimelineInputBoundaryKind::None,
        };
        m_hasPreparedInputBoundary = result.frameCount > 0U;
        m_preparedInputFrameCount  = result.frameCount;
        return result;
    }
}

AudioTimelinePlaybackState AudioTimelineMixerNode::state() const noexcept
{
    return m_publishedState.load(std::memory_order_relaxed);
}

AudioTimelinePlaybackState
AudioTimelineMixerNode::requestedState() const noexcept
{
    if ( m_publishedFinished.load(std::memory_order_relaxed) ) {
        return AudioTimelinePlaybackState::Stopped;
    }
    switch ( m_requestedPlaybackCommand.load(std::memory_order_relaxed) ) {
    case PlaybackCommand::Stop: return AudioTimelinePlaybackState::Stopped;
    case PlaybackCommand::Play: return AudioTimelinePlaybackState::Playing;
    case PlaybackCommand::Pause: return AudioTimelinePlaybackState::Paused;
    }
    return AudioTimelinePlaybackState::Stopped;
}

std::uint64_t AudioTimelineMixerNode::epoch() const noexcept
{
    return m_publishedEpoch.load(std::memory_order_relaxed);
}

bool AudioTimelineMixerNode::finished() const noexcept
{
    return m_publishedFinished.load(std::memory_order_relaxed);
}

AudioTimelineFrame AudioTimelineMixerNode::timelineEndFrame() const noexcept
{
    return m_publishedTimelineEndFrame.load(std::memory_order_relaxed);
}

std::size_t AudioTimelineMixerNode::clipCount() const noexcept
{
    return m_publishedClipCount.load(std::memory_order_relaxed);
}

void AudioTimelineMixerNode::setMasterGain(float gain) noexcept
{
    m_masterGain.store(std::isfinite(gain) ? std::max(gain, 0.0F) : 0.0F,
                       std::memory_order_relaxed);
}

float AudioTimelineMixerNode::masterGain() const noexcept
{
    return m_masterGain.load(std::memory_order_relaxed);
}

float AudioTimelineMixerNode::leftLevel() const noexcept
{
    return m_leftLevel.load(std::memory_order_relaxed);
}

float AudioTimelineMixerNode::rightLevel() const noexcept
{
    return m_rightLevel.load(std::memory_order_relaxed);
}

void AudioTimelineMixerNode::process(ice::AudioBuffer& buffer)
{
    buffer.clear();
    const bool consumePreparedBoundary =
        m_hasPreparedInputBoundary &&
        m_preparedInputFrameCount == buffer.num_frames();
    m_hasPreparedInputBoundary = false;
    m_preparedInputFrameCount  = 0U;
    if ( !consumePreparedBoundary ) {
        applyPendingSchedule();
        applyPendingControls();
    }
    m_publishedBlockStartFrame.store(
        m_scheduleState ? m_scheduleState->transport.positionFrame() : 0,
        std::memory_order_relaxed);

    if ( !m_scheduleState ||
         m_scheduleState->transport.state() !=
             AudioTimelinePlaybackState::Playing ||
         buffer.num_frames() == 0U ) {
        applyMasterGainAndPublishLevels(buffer);
        publishTransportSnapshot();
        return;
    }

    auto&       schedule         = *m_scheduleState;
    std::size_t outputStartFrame = 0U;
    while ( outputStartFrame < buffer.num_frames() ) {
        const auto loopRange = schedule.transport.loopRange();
        const auto position  = schedule.transport.positionFrame();

        if ( !loopRange && position >= schedule.timelineEndFrame ) {
            schedule.transport.pause();
            markFinishedAndNotify();
            break;
        }

        std::size_t frameCount =
            std::min(schedule.maximumProcessFrames,
                     buffer.num_frames() - outputStartFrame);

        if ( loopRange && position < loopRange->endFrame ) {
            const auto loopFrames = framesUntil(position, loopRange->endFrame);
            frameCount =
                std::min(frameCount, static_cast<std::size_t>(loopFrames));
        } else if ( !loopRange ) {
            const auto timelineFrames =
                framesUntil(position, schedule.timelineEndFrame);
            frameCount =
                std::min(frameCount, static_cast<std::size_t>(timelineFrames));
        }

        if ( frameCount == 0U ) {
            if ( loopRange ) {
                schedule.transport.seek(loopRange->startFrame);
                continue;
            }
            schedule.transport.pause();
            markFinishedAndNotify();
            break;
        }

        mixSegment(buffer, outputStartFrame, frameCount);
        outputStartFrame += frameCount;
    }

    if ( !schedule.transport.loopRange() &&
         schedule.transport.state() == AudioTimelinePlaybackState::Playing &&
         schedule.transport.positionFrame() >= schedule.timelineEndFrame ) {
        schedule.transport.pause();
        markFinishedAndNotify();
    }

    applyMasterGainAndPublishLevels(buffer);
    publishTransportSnapshot();
}

std::vector<PreparedTimelineClip> AudioTimelineMixerNode::prepareClips(
    std::vector<PreparedTimelineClip> clips)
{
    std::erase_if(clips, [](const PreparedTimelineClip& clip) {
        return !clip.audio || clip.audio->numFrames() == 0U;
    });
    for ( auto& clip : clips ) {
        clip.volume =
            std::isfinite(clip.volume) ? std::max(clip.volume, 0.0F) : 1.0F;
    }
    std::stable_sort(
        clips.begin(),
        clips.end(),
        [](const PreparedTimelineClip& lhs, const PreparedTimelineClip& rhs) {
            if ( lhs.startFrame != rhs.startFrame ) {
                return lhs.startFrame < rhs.startFrame;
            }
            return lhs.eventId < rhs.eventId;
        });
    return clips;
}

std::vector<TimelineClipSpec> AudioTimelineMixerNode::buildClipSpecs(
    const std::vector<PreparedTimelineClip>& clips)
{
    std::vector<TimelineClipSpec> specs;
    specs.reserve(clips.size());
    for ( const auto& clip : clips ) {
        specs.push_back(TimelineClipSpec{
            .eventId        = clip.eventId,
            .sourceKey      = clip.sourceKey,
            .startFrame     = clip.startFrame,
            .durationFrames = frameCountFromSize(clip.audio->numFrames()),
            .volume         = clip.volume,
        });
    }
    return specs;
}

AudioTimelineFrame AudioTimelineMixerNode::calculateTimelineEndFrame(
    const std::vector<PreparedTimelineClip>& clips,
    AudioTimelineFrame                       requestedTimelineEndFrame) noexcept
{
    auto endFrame = std::max<AudioTimelineFrame>(requestedTimelineEndFrame, 0);
    for ( const auto& clip : clips ) {
        endFrame = std::max(
            endFrame,
            saturatingFrameAdd(clip.startFrame,
                               frameCountFromSize(clip.audio->numFrames())));
    }
    return endFrame;
}

void AudioTimelineMixerNode::applyPendingSchedule() noexcept
{
    ScheduleState* replacement =
        m_pendingSchedule.exchange(nullptr, std::memory_order_acquire);
    if ( !replacement ) return;

    ScheduleState* previous = m_scheduleState;
    m_scheduleState         = replacement;
    initializeReplacementTransport(*replacement);
    retireSchedule(previous);
}

void AudioTimelineMixerNode::retireSchedule(ScheduleState* state) noexcept
{
    if ( !state ) return;

    ScheduleState* retiredHead =
        m_retiredSchedules.load(std::memory_order_relaxed);
    do {
        state->nextRetired = retiredHead;
    } while (
        !m_retiredSchedules.compare_exchange_weak(retiredHead,
                                                  state,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed) );
}

void AudioTimelineMixerNode::initializeReplacementTransport(
    ScheduleState& state) noexcept
{
    const auto loopSequence = m_loopSequence.load(std::memory_order_acquire);
    if ( (loopSequence & 1U) == 0U ) {
        const bool enabled =
            m_requestedLoopEnabled.load(std::memory_order_relaxed);
        const auto startFrame =
            m_requestedLoopStartFrame.load(std::memory_order_relaxed);
        const auto endFrame =
            m_requestedLoopEndFrame.load(std::memory_order_relaxed);
        if ( m_loopSequence.load(std::memory_order_acquire) == loopSequence ) {
            if ( enabled ) {
                static_cast<void>(
                    state.transport.setLoop({ startFrame, endFrame }));
            }
            m_appliedLoopSequence = loopSequence;
        }
    }

    AudioTimelineFrame replacementPosition =
        m_publishedPositionFrame.load(std::memory_order_relaxed);
    const auto seekSequence = m_seekSequence.load(std::memory_order_acquire);
    if ( hasStableUpdate(seekSequence, m_appliedSeekSequence) ) {
        const auto requestedPosition =
            m_requestedSeekFrame.load(std::memory_order_relaxed);
        if ( m_seekSequence.load(std::memory_order_acquire) == seekSequence ) {
            replacementPosition   = requestedPosition;
            m_appliedSeekSequence = seekSequence;
        }
    }
    state.transport.seek(replacementPosition);

    const auto playbackSequence =
        m_playbackCommandSequence.load(std::memory_order_acquire);
    if ( (playbackSequence & 1U) != 0U ) return;
    const auto command =
        m_requestedPlaybackCommand.load(std::memory_order_relaxed);
    if ( m_playbackCommandSequence.load(std::memory_order_acquire) !=
         playbackSequence ) {
        return;
    }

    if ( m_publishedFinished.load(std::memory_order_relaxed) ) {
        state.transport.stop();
        m_appliedPlaybackCommandSequence = playbackSequence;
        return;
    }

    switch ( command ) {
    case PlaybackCommand::Stop: state.transport.stop(); break;
    case PlaybackCommand::Play:
        if ( !state.transport.loopRange() &&
             state.transport.positionFrame() >= state.timelineEndFrame ) {
            state.transport.seek(0);
        }
        state.transport.play();
        break;
    case PlaybackCommand::Pause:
        state.transport.play();
        state.transport.pause();
        break;
    }
    m_appliedPlaybackCommandSequence = playbackSequence;
}

void AudioTimelineMixerNode::markFinishedAndNotify() noexcept
{
    const bool wasFinished =
        m_publishedFinished.exchange(true, std::memory_order_relaxed);
    if ( !wasFinished && m_finalInputListener ) {
        m_finalInputListener(m_finalInputListenerContext);
    }
}

void AudioTimelineMixerNode::applyPendingControls() noexcept
{
    if ( !m_scheduleState ) return;
    auto& transport = m_scheduleState->transport;

    const auto loopSequence = m_loopSequence.load(std::memory_order_acquire);
    if ( hasStableUpdate(loopSequence, m_appliedLoopSequence) ) {
        const bool enabled =
            m_requestedLoopEnabled.load(std::memory_order_relaxed);
        const auto startFrame =
            m_requestedLoopStartFrame.load(std::memory_order_relaxed);
        const auto endFrame =
            m_requestedLoopEndFrame.load(std::memory_order_relaxed);
        if ( m_loopSequence.load(std::memory_order_acquire) == loopSequence ) {
            if ( enabled ) {
                static_cast<void>(transport.setLoop({ startFrame, endFrame }));
            } else {
                transport.clearLoop();
            }
            m_appliedLoopSequence = loopSequence;
        }
    }

    const auto seekSequence = m_seekSequence.load(std::memory_order_acquire);
    if ( hasStableUpdate(seekSequence, m_appliedSeekSequence) ) {
        const auto frame = m_requestedSeekFrame.load(std::memory_order_relaxed);
        if ( m_seekSequence.load(std::memory_order_acquire) == seekSequence ) {
            transport.seek(frame);
            if ( m_requestedPlaybackCommand.load(std::memory_order_relaxed) ==
                 PlaybackCommand::Play ) {
                transport.play();
            }
            m_appliedSeekSequence = seekSequence;
            m_publishedFinished.store(false, std::memory_order_relaxed);
        }
    }

    const auto playbackSequence =
        m_playbackCommandSequence.load(std::memory_order_acquire);
    if ( !hasStableUpdate(playbackSequence,
                          m_appliedPlaybackCommandSequence) ) {
        return;
    }

    const auto command =
        m_requestedPlaybackCommand.load(std::memory_order_relaxed);
    if ( m_playbackCommandSequence.load(std::memory_order_acquire) !=
         playbackSequence ) {
        return;
    }

    switch ( command ) {
    case PlaybackCommand::Stop:
        transport.stop();
        m_publishedFinished.store(false, std::memory_order_relaxed);
        break;
    case PlaybackCommand::Play:
        if ( !transport.loopRange() &&
             transport.positionFrame() >= m_scheduleState->timelineEndFrame ) {
            transport.seek(0);
        }
        transport.play();
        m_publishedFinished.store(false, std::memory_order_relaxed);
        break;
    case PlaybackCommand::Pause: transport.pause(); break;
    }
    m_appliedPlaybackCommandSequence = playbackSequence;
}

void AudioTimelineMixerNode::publishTransportSnapshot() noexcept
{
    if ( !m_scheduleState ) {
        m_publishedPositionFrame.store(0, std::memory_order_relaxed);
        m_publishedState.store(AudioTimelinePlaybackState::Stopped,
                               std::memory_order_relaxed);
        return;
    }
    m_publishedPositionFrame.store(m_scheduleState->transport.positionFrame(),
                                   std::memory_order_relaxed);
    m_publishedState.store(m_scheduleState->transport.state(),
                           std::memory_order_relaxed);
    m_publishedEpoch.store(m_scheduleState->transport.epoch(),
                           std::memory_order_relaxed);
    m_publishedAppliedSeekSequence.store(m_appliedSeekSequence,
                                         std::memory_order_release);
}

void AudioTimelineMixerNode::mixSegment(ice::AudioBuffer& output,
                                        std::size_t       outputStartFrame,
                                        std::size_t       frameCount)
{
    if ( !m_scheduleState ) return;
    auto&      schedule = *m_scheduleState;
    const auto result   = schedule.transport.consumeActiveSpans(
        frameCountFromSize(frameCount), std::span(schedule.activeSpanScratch));
    if ( result.truncated ) return;

    const auto outputChannels = output.num_channels();
    for ( std::size_t spanIndex = 0U; spanIndex < result.writtenSpanCount;
          ++spanIndex ) {
        const auto& span = schedule.activeSpanScratch[spanIndex];
        const auto& clip = schedule.clips[span.clipIndex];
        if ( span.volume <= 0.0F ) continue;

        schedule.sourceScratch.clear();
        const auto requestedFrames = static_cast<std::size_t>(span.frameCount);
        const auto readFrames      = std::min<std::size_t>(
            clip.audio->read(schedule.sourceScratch,
                             static_cast<std::size_t>(span.sourceStartFrame),
                             requestedFrames),
            requestedFrames);
        const auto destinationStart =
            outputStartFrame + static_cast<std::size_t>(span.outputStartFrame);
        const auto channels = std::min<std::size_t>(
            outputChannels, schedule.sourceScratch.num_channels());

        for ( std::size_t channel = 0U; channel < channels; ++channel ) {
            float* destination  = output.raw_ptrs()[channel] + destinationStart;
            const float* source = schedule.sourceScratch.raw_ptrs()[channel];
            for ( std::size_t frame = 0U; frame < readFrames; ++frame ) {
                destination[frame] += source[frame] * span.volume;
            }
        }
    }
}

void AudioTimelineMixerNode::applyMasterGainAndPublishLevels(
    ice::AudioBuffer& output) noexcept
{
    if ( output.num_frames() == 0U || output.raw_ptrs() == nullptr ) {
        m_leftLevel.store(0.0F, std::memory_order_relaxed);
        m_rightLevel.store(0.0F, std::memory_order_relaxed);
        return;
    }

    const float       gain      = m_masterGain.load(std::memory_order_relaxed);
    const std::size_t channels  = output.num_channels();
    float             leftPeak  = 0.0F;
    float             rightPeak = 0.0F;

    for ( std::size_t channel = 0U; channel < channels; ++channel ) {
        float* channelData = output.raw_ptrs()[channel];
        float  channelPeak = 0.0F;
        for ( std::size_t frame = 0U; frame < output.num_frames(); ++frame ) {
            channelData[frame] *= gain;
            channelPeak = std::max(channelPeak, std::abs(channelData[frame]));
        }
        if ( channel == 0U ) {
            leftPeak = channelPeak;
        } else if ( channel == 1U ) {
            rightPeak = channelPeak;
        }
    }
    if ( channels == 1U ) rightPeak = leftPeak;
    m_leftLevel.store(leftPeak, std::memory_order_relaxed);
    m_rightLevel.store(rightPeak, std::memory_order_relaxed);
}

void AudioTimelineMixerNode::requestPlaybackCommand(
    PlaybackCommand command) noexcept
{
    m_playbackCommandSequence.fetch_add(1U, std::memory_order_acq_rel);
    m_requestedPlaybackCommand.store(command, std::memory_order_relaxed);
    m_playbackCommandSequence.fetch_add(1U, std::memory_order_release);
}

}  // namespace MMM::Audio
