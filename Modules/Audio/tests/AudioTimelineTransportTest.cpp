#include "audio/AudioTimelineTransport.h"

#include "log/colorful-log.h"

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace
{

using MMM::Audio::AudioTimelineActiveSpan;
using MMM::Audio::AudioTimelineLoopRange;
using MMM::Audio::AudioTimelinePlaybackState;
using MMM::Audio::AudioTimelineTransport;
using MMM::Audio::TimelineClipSpec;

/// @brief 记录断言失败并返回可累计的失败数量。
/// @param condition 需要成立的条件。
/// @param label 失败时输出的测试标签。
/// @return 条件成立时返回 0，否则返回 1。
int expectTrue(bool condition, std::string_view label)
{
    if ( condition ) return 0;
    XERROR("AudioTimelineTransport assertion failed: {}", label);
    return 1;
}

/// @brief 验证调度表排序、无效片段过滤和负起点裁切。
/// @return 失败断言数量。
int testSortedScheduleAndNegativeStart()
{
    AudioTimelineTransport transport({
        TimelineClipSpec{
            .eventId        = 30U,
            .sourceKey      = "late-b",
            .startFrame     = 10,
            .durationFrames = 5,
            .volume         = 0.3F,
        },
        TimelineClipSpec{
            .eventId        = 20U,
            .sourceKey      = "negative",
            .startFrame     = -5,
            .durationFrames = 10,
            .volume         = 0.5F,
        },
        TimelineClipSpec{
            .eventId        = 10U,
            .sourceKey      = "late-a",
            .startFrame     = 10,
            .durationFrames = 5,
            .volume         = 0.1F,
        },
        TimelineClipSpec{
            .eventId        = 40U,
            .sourceKey      = "ignored",
            .startFrame     = 0,
            .durationFrames = 0,
            .volume         = 1.0F,
        },
    });

    int        failures = 0;
    const auto clips    = transport.clips();
    failures += expectTrue(clips.size() == 3U,
                           "non-positive duration clip is filtered");
    failures +=
        expectTrue(clips.size() == 3U && clips[0].eventId == 20U &&
                       clips[1].eventId == 10U && clips[2].eventId == 30U,
                   "clips are sorted by start frame then event id");

    std::array<AudioTimelineActiveSpan, 4U> spans;
    const auto result = transport.queryActiveSpans(0, 10, spans);
    failures += expectTrue(result.writtenSpanCount == 1U &&
                               result.totalSpanCount == 1U && !result.truncated,
                           "negative-start query returns one complete span");
    failures += expectTrue(
        spans[0].eventId == 20U && spans[0].sourceKey == "negative" &&
            spans[0].outputStartFrame == 0 && spans[0].sourceStartFrame == 5 &&
            spans[0].frameCount == 5 && spans[0].volume == 0.5F,
        "negative-start span begins from the elapsed source frame");
    return failures;
}

/// @brief 验证播放、暂停、seek、stop 和纪元变更语义。
/// @return 失败断言数量。
int testPlaybackStateSeekAndEpoch()
{
    AudioTimelineTransport transport({
        TimelineClipSpec{
            .eventId        = 1U,
            .sourceKey      = "long",
            .startFrame     = 0,
            .durationFrames = 100,
            .volume         = 1.0F,
        },
    });

    int                                     failures = 0;
    std::array<AudioTimelineActiveSpan, 2U> spans;
    const auto stoppedResult = transport.consumeActiveSpans(10, spans);
    failures += expectTrue(
        stoppedResult.totalSpanCount == 0U && transport.positionFrame() == 0 &&
            transport.state() == AudioTimelinePlaybackState::Stopped,
        "stopped transport does not advance");

    transport.play();
    const auto playingResult = transport.consumeActiveSpans(10, spans);
    failures += expectTrue(
        playingResult.writtenSpanCount == 1U &&
            spans[0].sourceStartFrame == 0 && spans[0].frameCount == 10 &&
            transport.positionFrame() == 10 && transport.epoch() == 0U,
        "playing transport consumes source frames");

    transport.pause();
    const auto pausedResult = transport.consumeActiveSpans(10, spans);
    failures += expectTrue(
        pausedResult.totalSpanCount == 0U && transport.positionFrame() == 10 &&
            transport.state() == AudioTimelinePlaybackState::Paused,
        "paused transport freezes position");

    transport.seek(25);
    failures +=
        expectTrue(transport.positionFrame() == 25 && transport.epoch() == 1U &&
                       transport.state() == AudioTimelinePlaybackState::Paused,
                   "seek preserves pause state and opens a new epoch");
    transport.play();
    const auto seekResult = transport.consumeActiveSpans(5, spans);
    failures += expectTrue(
        seekResult.writtenSpanCount == 1U && spans[0].sourceStartFrame == 25 &&
            spans[0].frameCount == 5 && spans[0].epoch == 1U &&
            transport.positionFrame() == 30,
        "seek inside a clip resumes from the matching source frame");

    transport.seek(25);
    failures += expectTrue(
        transport.positionFrame() == 25 && transport.epoch() == 2U,
        "repeated explicit seek opens a deterministic rebuild epoch");

    transport.stop();
    failures +=
        expectTrue(transport.positionFrame() == 0 && transport.epoch() == 3U &&
                       transport.state() == AudioTimelinePlaybackState::Stopped,
                   "stop resets position and opens a new epoch");
    return failures;
}

/// @brief 验证半开循环边界、跨界裁切与循环起点重建。
/// @return 失败断言数量。
int testHalfOpenLoopAndEpoch()
{
    AudioTimelineTransport transport({
        TimelineClipSpec{
            .eventId        = 4U,
            .sourceKey      = "overlap",
            .startFrame     = 5,
            .durationFrames = 20,
            .volume         = 1.0F,
        },
        TimelineClipSpec{
            .eventId        = 1U,
            .sourceKey      = "at-loop-start",
            .startFrame     = 10,
            .durationFrames = 2,
            .volume         = 1.0F,
        },
        TimelineClipSpec{
            .eventId        = 2U,
            .sourceKey      = "cross-loop-end",
            .startFrame     = 18,
            .durationFrames = 5,
            .volume         = 1.0F,
        },
        TimelineClipSpec{
            .eventId        = 3U,
            .sourceKey      = "at-excluded-end",
            .startFrame     = 20,
            .durationFrames = 2,
            .volume         = 1.0F,
        },
    });

    int failures = 0;
    failures += expectTrue(!transport.setLoop(AudioTimelineLoopRange{
                               .startFrame = 20,
                               .endFrame   = 20,
                           }),
                           "empty loop range is rejected");
    failures += expectTrue(transport.setLoop(AudioTimelineLoopRange{
                               .startFrame = 10,
                               .endFrame   = 20,
                           }),
                           "valid half-open loop range is accepted");

    transport.seek(18);
    transport.play();
    std::array<AudioTimelineActiveSpan, 8U> spans;
    const auto result = transport.consumeActiveSpans(6, spans);
    failures +=
        expectTrue(result.writtenSpanCount == 4U && !result.truncated,
                   "consume splits active spans across the loop boundary");
    failures +=
        expectTrue(spans[0].eventId == 4U && spans[0].outputStartFrame == 0 &&
                       spans[0].sourceStartFrame == 13 &&
                       spans[0].frameCount == 2 && spans[0].epoch == 1U,
                   "long overlap clip is cut at the loop end");
    failures +=
        expectTrue(spans[1].eventId == 2U && spans[1].outputStartFrame == 0 &&
                       spans[1].sourceStartFrame == 0 &&
                       spans[1].frameCount == 2 && spans[1].epoch == 1U,
                   "clip crossing the loop end only emits its in-range head");
    failures += expectTrue(
        spans[2].eventId == 4U && spans[2].outputStartFrame == 2 &&
            spans[2].sourceStartFrame == 5 && spans[2].frameCount == 4 &&
            spans[2].epoch == 2U,
        "loop start rebuilds a clip already active at the inclusive boundary");
    failures += expectTrue(
        spans[3].eventId == 1U && spans[3].outputStartFrame == 2 &&
            spans[3].sourceStartFrame == 0 && spans[3].frameCount == 2 &&
            spans[3].epoch == 2U,
        "event at the inclusive loop start is emitted in the new epoch");
    failures +=
        expectTrue(transport.positionFrame() == 14 && transport.epoch() == 2U,
                   "loop consumption advances from the inclusive start");

    for ( std::size_t index = 0U; index < result.writtenSpanCount; ++index ) {
        failures += expectTrue(spans[index].eventId != 3U,
                               "event at the excluded loop end is not emitted");
    }

    transport.seek(18);
    const auto exactBoundaryResult = transport.consumeActiveSpans(2, spans);
    failures += expectTrue(
        exactBoundaryResult.totalSpanCount == 2U &&
            transport.positionFrame() == 10 && transport.epoch() == 4U,
        "ending exactly at loop end wraps immediately into a new epoch");
    return failures;
}

/// @brief 验证小输出缓冲区仍报告完整数量并确定性推进传输。
/// @return 失败断言数量。
int testTruncatedOutputBuffer()
{
    AudioTimelineTransport transport({
        TimelineClipSpec{
            .eventId        = 1U,
            .sourceKey      = "a",
            .startFrame     = 0,
            .durationFrames = 20,
            .volume         = 1.0F,
        },
        TimelineClipSpec{
            .eventId        = 2U,
            .sourceKey      = "b",
            .startFrame     = 0,
            .durationFrames = 20,
            .volume         = 1.0F,
        },
    });

    std::array<AudioTimelineActiveSpan, 1U> spans;
    const auto queryResult = transport.queryActiveSpans(0, 10, spans);
    int failures = expectTrue(queryResult.writtenSpanCount == 1U &&
                                  queryResult.totalSpanCount == 2U &&
                                  queryResult.truncated,
                              "query reports truncation without allocating");

    transport.play();
    const auto consumeResult = transport.consumeActiveSpans(10, spans);
    failures += expectTrue(
        consumeResult.writtenSpanCount == 1U &&
            consumeResult.totalSpanCount == 2U && consumeResult.truncated &&
            transport.positionFrame() == 10,
        "truncated consume still advances the full requested range");
    return failures;
}

}  // namespace

/// @brief 运行纯逻辑音频时间线传输测试。
/// @return 所有断言通过时返回 0。
int main()
{
    XLogger::init("AudioTimelineTransportTest");

    int failures = 0;
    failures += testSortedScheduleAndNegativeStart();
    failures += testPlaybackStateSeekAndEpoch();
    failures += testHalfOpenLoopAndEpoch();
    failures += testTruncatedOutputBuffer();

    if ( failures != 0 ) {
        XERROR("AudioTimelineTransportTest failed with {} assertion(s)",
               failures);
        return 1;
    }
    XINFO("AudioTimelineTransportTest passed");
    return 0;
}
