#include "audio/AudioTimelineTransport.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace MMM::Audio
{
namespace
{

/// @brief 对有符号时间线帧执行上界饱和加法。
/// @param frame 起始帧。
/// @param positiveDelta 非负增量。
/// @return 不超过 AudioTimelineFrame 最大值的计算结果。
[[nodiscard]] AudioTimelineFrame saturatingAdd(
    AudioTimelineFrame frame, AudioTimelineFrame positiveDelta) noexcept
{
    if ( positiveDelta <= 0 ) return frame;

    constexpr AudioTimelineFrame MAX_FRAME =
        std::numeric_limits<AudioTimelineFrame>::max();
    if ( frame > MAX_FRAME - positiveDelta ) return MAX_FRAME;
    return frame + positiveDelta;
}

/// @brief 在不执行有符号溢出运算的前提下限制到指定排除边界。
/// @param currentFrame 当前帧，必须小于 boundaryFrame。
/// @param boundaryFrame 排除边界。
/// @param requestedFrames 本次最多消费的非负帧数。
/// @return requestedFrames 与边界距离中的较小值。
[[nodiscard]] AudioTimelineFrame framesBeforeBoundary(
    AudioTimelineFrame currentFrame, AudioTimelineFrame boundaryFrame,
    AudioTimelineFrame requestedFrames) noexcept
{
    if ( requestedFrames <= 0 || currentFrame >= boundaryFrame ) return 0;

    const auto unsignedDistance = static_cast<std::uint64_t>(boundaryFrame) -
                                  static_cast<std::uint64_t>(currentFrame);
    if ( unsignedDistance >= static_cast<std::uint64_t>(requestedFrames) ) {
        return requestedFrames;
    }
    return static_cast<AudioTimelineFrame>(unsignedDistance);
}

/// @brief 判断两个循环范围是否完全一致。
/// @param lhs 左侧范围。
/// @param rhs 右侧范围。
/// @return 起止帧都相同时返回 true。
[[nodiscard]] bool loopRangesEqual(const AudioTimelineLoopRange& lhs,
                                   const AudioTimelineLoopRange& rhs) noexcept
{
    return lhs.startFrame == rhs.startFrame && lhs.endFrame == rhs.endFrame;
}

}  // namespace

AudioTimelineTransport::AudioTimelineTransport(
    std::vector<TimelineClipSpec> clips)
{
    std::erase_if(clips, [](const TimelineClipSpec& clip) {
        return clip.durationFrames <= 0;
    });
    std::stable_sort(
        clips.begin(),
        clips.end(),
        [](const TimelineClipSpec& lhs, const TimelineClipSpec& rhs) {
            if ( lhs.startFrame != rhs.startFrame ) {
                return lhs.startFrame < rhs.startFrame;
            }
            return lhs.eventId < rhs.eventId;
        });

    m_clips = std::move(clips);
    m_clipEndFrames.reserve(m_clips.size());
    m_prefixMaxEndFrames.reserve(m_clips.size());

    AudioTimelineFrame prefixMaxEnd =
        std::numeric_limits<AudioTimelineFrame>::min();
    for ( const auto& clip : m_clips ) {
        const auto endFrame =
            saturatingAdd(clip.startFrame, clip.durationFrames);
        m_clipEndFrames.push_back(endFrame);
        prefixMaxEnd = std::max(prefixMaxEnd, endFrame);
        m_prefixMaxEndFrames.push_back(prefixMaxEnd);
    }
}

std::span<const TimelineClipSpec> AudioTimelineTransport::clips() const noexcept
{
    return m_clips;
}

AudioTimelinePlaybackState AudioTimelineTransport::state() const noexcept
{
    return m_state;
}

AudioTimelineFrame AudioTimelineTransport::positionFrame() const noexcept
{
    return m_positionFrame;
}

std::uint64_t AudioTimelineTransport::epoch() const noexcept
{
    return m_epoch;
}

const std::optional<AudioTimelineLoopRange>&
AudioTimelineTransport::loopRange() const noexcept
{
    return m_loopRange;
}

void AudioTimelineTransport::play() noexcept
{
    m_state = AudioTimelinePlaybackState::Playing;
}

void AudioTimelineTransport::pause() noexcept
{
    if ( m_state == AudioTimelinePlaybackState::Playing ) {
        m_state = AudioTimelinePlaybackState::Paused;
    }
}

void AudioTimelineTransport::stop() noexcept
{
    m_state         = AudioTimelinePlaybackState::Stopped;
    m_positionFrame = 0;
    ++m_epoch;
}

void AudioTimelineTransport::seek(AudioTimelineFrame frame) noexcept
{
    if ( m_loopRange && frame >= m_loopRange->endFrame ) {
        m_positionFrame = m_loopRange->startFrame;
    } else {
        m_positionFrame = frame;
    }
    ++m_epoch;
}

bool AudioTimelineTransport::setLoop(AudioTimelineLoopRange range) noexcept
{
    if ( range.startFrame >= range.endFrame ) return false;

    const bool rangeChanged =
        !m_loopRange || !loopRangesEqual(*m_loopRange, range);
    m_loopRange = range;

    if ( m_positionFrame >= range.endFrame ) {
        m_positionFrame = range.startFrame;
        if ( rangeChanged ) ++m_epoch;
    }
    return true;
}

void AudioTimelineTransport::clearLoop() noexcept
{
    m_loopRange.reset();
}

AudioTimelineSpanQueryResult AudioTimelineTransport::queryActiveSpans(
    AudioTimelineFrame startFrame, AudioTimelineFrame frameCount,
    std::span<AudioTimelineActiveSpan> output) const noexcept
{
    AudioTimelineSpanQueryResult result;
    appendActiveSpans(startFrame, frameCount, 0, m_epoch, output, result);
    result.truncated = result.writtenSpanCount < result.totalSpanCount;
    return result;
}

AudioTimelineSpanQueryResult AudioTimelineTransport::consumeActiveSpans(
    AudioTimelineFrame                 frameCount,
    std::span<AudioTimelineActiveSpan> output) noexcept
{
    AudioTimelineSpanQueryResult result;
    if ( frameCount <= 0 || m_state != AudioTimelinePlaybackState::Playing ) {
        return result;
    }

    AudioTimelineFrame remainingFrames  = frameCount;
    AudioTimelineFrame outputStartFrame = 0;

    while ( remainingFrames > 0 ) {
        if ( m_loopRange && m_positionFrame >= m_loopRange->endFrame ) {
            m_positionFrame = m_loopRange->startFrame;
            ++m_epoch;
        }

        AudioTimelineFrame segmentFrameCount = remainingFrames;
        if ( m_loopRange && m_positionFrame < m_loopRange->endFrame ) {
            segmentFrameCount = framesBeforeBoundary(
                m_positionFrame, m_loopRange->endFrame, segmentFrameCount);
        }

        appendActiveSpans(m_positionFrame,
                          segmentFrameCount,
                          outputStartFrame,
                          m_epoch,
                          output,
                          result);

        m_positionFrame = saturatingAdd(m_positionFrame, segmentFrameCount);
        remainingFrames -= segmentFrameCount;
        outputStartFrame += segmentFrameCount;

        if ( m_loopRange && m_positionFrame == m_loopRange->endFrame ) {
            m_positionFrame = m_loopRange->startFrame;
            ++m_epoch;
        }
    }

    result.truncated = result.writtenSpanCount < result.totalSpanCount;
    return result;
}

void AudioTimelineTransport::appendActiveSpans(
    AudioTimelineFrame startFrame, AudioTimelineFrame frameCount,
    AudioTimelineFrame outputStartFrame, std::uint64_t queryEpoch,
    std::span<AudioTimelineActiveSpan> output,
    AudioTimelineSpanQueryResult&      result) const noexcept
{
    if ( frameCount <= 0 || m_clips.empty() ) return;

    const AudioTimelineFrame endFrame = saturatingAdd(startFrame, frameCount);
    if ( endFrame <= startFrame ) return;

    const auto firstCandidate = std::upper_bound(
        m_prefixMaxEndFrames.begin(), m_prefixMaxEndFrames.end(), startFrame);
    const auto lastCandidate = std::lower_bound(
        m_clips.begin(),
        m_clips.end(),
        endFrame,
        [](const TimelineClipSpec& clip, AudioTimelineFrame frame) {
            return clip.startFrame < frame;
        });

    const std::size_t firstIndex = static_cast<std::size_t>(
        std::distance(m_prefixMaxEndFrames.begin(), firstCandidate));
    const std::size_t lastIndex =
        static_cast<std::size_t>(std::distance(m_clips.begin(), lastCandidate));

    for ( std::size_t index = firstIndex; index < lastIndex; ++index ) {
        const auto&              clip = m_clips[index];
        const AudioTimelineFrame overlapStart =
            std::max(startFrame, clip.startFrame);
        const AudioTimelineFrame overlapEnd =
            std::min(endFrame, m_clipEndFrames[index]);
        if ( overlapStart >= overlapEnd ) continue;

        if ( result.writtenSpanCount < output.size() ) {
            output[result.writtenSpanCount] = {
                .clipIndex = index,
                .eventId   = clip.eventId,
                .sourceKey = clip.sourceKey,
                .outputStartFrame =
                    outputStartFrame + (overlapStart - startFrame),
                .sourceStartFrame = overlapStart - clip.startFrame,
                .frameCount       = overlapEnd - overlapStart,
                .volume           = clip.volume,
                .epoch            = queryEpoch,
            };
            ++result.writtenSpanCount;
        }
        ++result.totalSpanCount;
    }
}

}  // namespace MMM::Audio
