#include "audio/AudioTimelineMixerNode.h"

#include <algorithm>
#include <cmath>
#include <ice/manage/AudioTrack.hpp>
#include <limits>
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

AudioTimelineMixerNode::AudioTimelineMixerNode(
    std::vector<PreparedTimelineClip> clips,
    AudioTimelineFrame                requestedTimelineEndFrame,
    std::size_t                       maximumProcessFrames)
    : m_clips(prepareClips(std::move(clips)))
    , m_transport(buildClipSpecs(m_clips))
    , m_timelineEndFrame(
          calculateTimelineEndFrame(m_clips, requestedTimelineEndFrame))
    , m_maximumProcessFrames(std::max<std::size_t>(maximumProcessFrames, 1U))
    , m_sourceScratch(ice::ICEConfig::internal_format, m_maximumProcessFrames)
    , m_activeSpanScratch(m_clips.size())
{
    publishTransportSnapshot();
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
    return m_publishedPositionFrame.load(std::memory_order_relaxed);
}

AudioTimelinePlaybackState AudioTimelineMixerNode::state() const noexcept
{
    return m_publishedState.load(std::memory_order_relaxed);
}

AudioTimelinePlaybackState
AudioTimelineMixerNode::requestedState() const noexcept
{
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
    return m_timelineEndFrame;
}

std::size_t AudioTimelineMixerNode::clipCount() const noexcept
{
    return m_clips.size();
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
    applyPendingControls();

    if ( m_transport.state() != AudioTimelinePlaybackState::Playing ||
         buffer.num_frames() == 0U ) {
        applyMasterGainAndPublishLevels(buffer);
        publishTransportSnapshot();
        return;
    }

    std::size_t outputStartFrame = 0U;
    while ( outputStartFrame < buffer.num_frames() ) {
        const auto loopRange = m_transport.loopRange();
        const auto position  = m_transport.positionFrame();

        if ( !loopRange && position >= m_timelineEndFrame ) {
            m_transport.pause();
            m_publishedFinished.store(true, std::memory_order_relaxed);
            break;
        }

        std::size_t frameCount = std::min(
            m_maximumProcessFrames, buffer.num_frames() - outputStartFrame);

        if ( loopRange && position < loopRange->endFrame ) {
            const auto loopFrames = framesUntil(position, loopRange->endFrame);
            frameCount =
                std::min(frameCount, static_cast<std::size_t>(loopFrames));
        } else if ( !loopRange ) {
            const auto timelineFrames =
                framesUntil(position, m_timelineEndFrame);
            frameCount =
                std::min(frameCount, static_cast<std::size_t>(timelineFrames));
        }

        if ( frameCount == 0U ) {
            if ( loopRange ) {
                m_transport.seek(loopRange->startFrame);
                continue;
            }
            m_transport.pause();
            m_publishedFinished.store(true, std::memory_order_relaxed);
            break;
        }

        mixSegment(buffer, outputStartFrame, frameCount);
        outputStartFrame += frameCount;
    }

    applyMasterGainAndPublishLevels(buffer);
    publishTransportSnapshot();
}

std::vector<PreparedTimelineClip> AudioTimelineMixerNode::prepareClips(
    std::vector<PreparedTimelineClip> clips)
{
    std::erase_if(clips, [](const PreparedTimelineClip& clip) {
        return !clip.track || clip.track->num_frames() == 0U;
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
            .durationFrames = frameCountFromSize(clip.track->num_frames()),
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
                               frameCountFromSize(clip.track->num_frames())));
    }
    return endFrame;
}

void AudioTimelineMixerNode::applyPendingControls() noexcept
{
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
                static_cast<void>(
                    m_transport.setLoop({ startFrame, endFrame }));
            } else {
                m_transport.clearLoop();
            }
            m_appliedLoopSequence = loopSequence;
            m_publishedFinished.store(false, std::memory_order_relaxed);
        }
    }

    const auto seekSequence = m_seekSequence.load(std::memory_order_acquire);
    if ( hasStableUpdate(seekSequence, m_appliedSeekSequence) ) {
        const auto frame = m_requestedSeekFrame.load(std::memory_order_relaxed);
        if ( m_seekSequence.load(std::memory_order_acquire) == seekSequence ) {
            m_transport.seek(frame);
            if ( m_requestedPlaybackCommand.load(std::memory_order_relaxed) ==
                 PlaybackCommand::Play ) {
                m_transport.play();
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
        m_transport.stop();
        m_publishedFinished.store(false, std::memory_order_relaxed);
        break;
    case PlaybackCommand::Play:
        if ( !m_transport.loopRange() &&
             m_transport.positionFrame() >= m_timelineEndFrame ) {
            m_transport.seek(0);
        }
        m_transport.play();
        m_publishedFinished.store(false, std::memory_order_relaxed);
        break;
    case PlaybackCommand::Pause: m_transport.pause(); break;
    }
    m_appliedPlaybackCommandSequence = playbackSequence;
}

void AudioTimelineMixerNode::publishTransportSnapshot() noexcept
{
    m_publishedPositionFrame.store(m_transport.positionFrame(),
                                   std::memory_order_relaxed);
    m_publishedState.store(m_transport.state(), std::memory_order_relaxed);
    m_publishedEpoch.store(m_transport.epoch(), std::memory_order_relaxed);
}

void AudioTimelineMixerNode::mixSegment(ice::AudioBuffer& output,
                                        std::size_t       outputStartFrame,
                                        std::size_t       frameCount)
{
    const auto result = m_transport.consumeActiveSpans(
        frameCountFromSize(frameCount), std::span(m_activeSpanScratch));
    if ( result.truncated ) return;

    const auto outputChannels = output.num_channels();
    for ( std::size_t spanIndex = 0U; spanIndex < result.writtenSpanCount;
          ++spanIndex ) {
        const auto& span = m_activeSpanScratch[spanIndex];
        const auto& clip = m_clips[span.clipIndex];
        if ( span.volume <= 0.0F ) continue;

        m_sourceScratch.clear();
        const auto requestedFrames = static_cast<std::size_t>(span.frameCount);
        const auto readFrames      = std::min<std::size_t>(
            clip.track->read(m_sourceScratch,
                             static_cast<std::size_t>(span.sourceStartFrame),
                             requestedFrames),
            requestedFrames);
        const auto destinationStart =
            outputStartFrame + static_cast<std::size_t>(span.outputStartFrame);
        const auto channels = std::min<std::size_t>(
            outputChannels, m_sourceScratch.num_channels());

        for ( std::size_t channel = 0U; channel < channels; ++channel ) {
            float* destination  = output.raw_ptrs()[channel] + destinationStart;
            const float* source = m_sourceScratch.raw_ptrs()[channel];
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
