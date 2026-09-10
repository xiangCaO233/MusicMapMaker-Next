#include "audio/AudioTimelineTransport.h"

#include "log/colorful-log.h"

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

/**
 * @file AudioTimelineTransportTest.cpp
 * @brief 验证纯逻辑音频时间线的排序、状态、循环、容量与查询复杂度。
 *
 * 测试不创建音频设备或 PCM，只检查 transport 将不可变片段调度成输出跨度的
 * 结果。所有帧区间采用半开语义，输出数组由测试预分配，因此这些场景同时
 * 固化未来音频回调所需的无分配契约。
 *
 * 覆盖层次如下：
 *
 * - 构造阶段过滤非正持续片段，并按起点、事件 ID 稳定排序；
 * - 任意区间查询裁切负起点片段，但不改变 transport 状态；
 * - 播放、暂停、seek 和 stop 按约定推进位置与 epoch；
 * - 循环端点为 [start, end)，跨界输出拆成两个不同 epoch；
 * - 输出容量不足只截断写入，不改变完整数量和时间线推进；
 * - 区间树在晚期稀疏查询中跳过大量已结束短片段。
 *
 * 每个测试返回失败断言数量，main 会继续运行后续场景，便于一次日志同时
 * 暴露多个独立回归。最终退出码仍由累计失败数量决定。
 *
 * 断言始终同时检查数量与关键跨度字段。只看 writtenSpanCount 无法区分正确
 * 片段和错误片段；只看 positionFrame 又会遗漏源偏移或 epoch 错位。性能场景
 * 使用访问节点数而非耗时，保持 Debug、RelWithDebInfo 和不同机器间稳定。
 *
 * 测试数组均在栈上固定容量，避免测试自身分配行为掩盖 transport 的接口约束。
 * 除专门的截断场景外，容量都大于预期跨度数；若新增片段使结果增多，应同时
 * 更新容量和完整数量断言，而不是只放宽 truncated。所有 sourceKey 都来自
 * transport 内部不可变字符串，断言只在 transport 生命周期内读取这些视图。
 * 新增循环用例还应同时验证返回位置和 epoch，避免只证明本次输出正确却让
 * 下一次回调从错误的连续状态开始。
 */

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
    // 成功不写日志，避免大量细粒度断言淹没真正失败原因。
    if ( condition ) return 0;
    XERROR("AudioTimelineTransport assertion failed: {}", label);
    return 1;
}

/**
 * @brief 验证调度表排序、无效片段过滤和负起点裁切。
 *
 * 输入故意乱序，两个片段共享 startFrame=10，并加入 duration=0 的无效项。
 * 负起点片段覆盖 [-5, 5)，查询 [0, 10) 时只能输出其后半段：输出偏移为零，
 * 源内偏移为 5，帧数为 5。
 *
 * @return 过滤、排序或裁切断言失败的累计数量。
 */
int testSortedScheduleAndNegativeStart()
{
    // late-b 先于 late-a 输入，验证同起点时 eventId 才是确定顺序。
    AudioTimelineTransport transport({
        // 负起点片段在时间线零点前已有五帧被消费。
        TimelineClipSpec{
            .eventId        = 30U,
            .sourceKey      = "late-b",
            .startFrame     = 10,
            .durationFrames = 5,
            .volume         = 0.3F,
        },
        // duration=0 永远不与半开查询相交，应在构造阶段移除。
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

    // 先检查构造后的公开调度表，再验证区间查询结果。
    int        failures = 0;
    const auto clips    = transport.clips();
    failures += expectTrue(clips.size() == 3U,
                           "non-positive duration clip is filtered");
    failures +=
        expectTrue(clips.size() == 3U && clips[0].eventId == 20U &&
                       clips[1].eventId == 10U && clips[2].eventId == 30U,
                   "clips are sorted by start frame then event id");

    // queryActiveSpans 不依赖 Playing 状态，可用于预览或离线规划。
    // 容量大于预期结果，保证本场景不触发截断分支。
    std::array<AudioTimelineActiveSpan, 4U> spans;
    const auto result = transport.queryActiveSpans(0, 10, spans);
    failures += expectTrue(result.writtenSpanCount == 1U &&
                               result.totalSpanCount == 1U && !result.truncated,
                           "negative-start query returns one complete span");
    // sourceStartFrame=5 是负起点裁切正确的关键证据。
    failures += expectTrue(
        spans[0].eventId == 20U && spans[0].sourceKey == "negative" &&
            spans[0].outputStartFrame == 0 && spans[0].sourceStartFrame == 5 &&
            spans[0].frameCount == 5 && spans[0].volume == 0.5F,
        "negative-start span begins from the elapsed source frame");
    return failures;
}

/**
 * @brief 验证播放、暂停、seek、stop 和纪元变更语义。
 *
 * 单一长片段排除多片段排序影响。Stopped 与 Paused 消费都不推进；Playing
 * 消费按请求增加位置。每次显式 seek 都开启新 epoch，即使目标位置相同；
 * stop 则同时归零位置、切换状态并开启下一 epoch。
 *
 * @return 状态、位置、源偏移或 epoch 断言失败的累计数量。
 */
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

    // 初始状态必须是 Stopped，首次消费用于验证完全无副作用。
    int                                     failures = 0;
    std::array<AudioTimelineActiveSpan, 2U> spans;
    const auto stoppedResult = transport.consumeActiveSpans(10, spans);
    // 空结果同时要求位置和状态不变，证明 stopped 消费没有隐藏推进。
    failures += expectTrue(
        stoppedResult.totalSpanCount == 0U && transport.positionFrame() == 0 &&
            transport.state() == AudioTimelinePlaybackState::Stopped,
        "stopped transport does not advance");

    // 首次播放从源帧零开始，普通连续消费不改变 epoch。
    transport.play();
    const auto playingResult = transport.consumeActiveSpans(10, spans);
    // span 的源起点、帧数、位置和 epoch 共同描述第一次连续消费。
    failures += expectTrue(
        playingResult.writtenSpanCount == 1U &&
            spans[0].sourceStartFrame == 0 && spans[0].frameCount == 10 &&
            transport.positionFrame() == 10 && transport.epoch() == 0U,
        "playing transport consumes source frames");

    // pause 保留位置，后续恢复应从同一连续区间继续。
    transport.pause();
    const auto pausedResult = transport.consumeActiveSpans(10, spans);
    // Paused 与 Stopped 都不消费，但暂停保留此前位置与恢复能力。
    failures += expectTrue(
        pausedResult.totalSpanCount == 0U && transport.positionFrame() == 10 &&
            transport.state() == AudioTimelinePlaybackState::Paused,
        "paused transport freezes position");

    // 暂停态 seek 只定位并递增 epoch，不应擅自开始播放。
    transport.seek(25);
    failures +=
        expectTrue(transport.positionFrame() == 25 && transport.epoch() == 1U &&
                       transport.state() == AudioTimelinePlaybackState::Paused,
                   "seek preserves pause state and opens a new epoch");
    // 恢复后跨度从源内第 25 帧开始，携带 seek 产生的新 epoch。
    transport.play();
    const auto seekResult = transport.consumeActiveSpans(5, spans);
    failures += expectTrue(
        seekResult.writtenSpanCount == 1U && spans[0].sourceStartFrame == 25 &&
            spans[0].frameCount == 5 && spans[0].epoch == 1U &&
            transport.positionFrame() == 30,
        "seek inside a clip resumes from the matching source frame");

    // 重复目标仍代表显式重建请求，因此 epoch 必须再次变化。
    transport.seek(25);
    failures += expectTrue(
        transport.positionFrame() == 25 && transport.epoch() == 2U,
        "repeated explicit seek opens a deterministic rebuild epoch");

    // stop 与 pause 不同：它归零位置并终止播放状态。
    transport.stop();
    failures +=
        expectTrue(transport.positionFrame() == 0 && transport.epoch() == 3U &&
                       transport.state() == AudioTimelinePlaybackState::Stopped,
                   "stop resets position and opens a new epoch");
    return failures;
}

/**
 * @brief 验证半开循环边界、跨界裁切与循环起点重建。
 *
 * 循环范围设为 [10, 20)，从 18 消费六帧。前两帧属于旧 epoch 的尾部，
 * 后四帧从 10 重新开始并属于新 epoch。跨越端点的长片段会在两段各输出
 * 一个跨度，而恰好从排除端点 20 开始的片段永远不出现。
 *
 * 场景还验证精确消费到 end 时立即回绕，调用返回位置为循环起点，而不是
 * 暂时停留在排除端点等待下一次调用修正。
 *
 * @return 循环范围、跨度裁切、输出偏移或 epoch 断言失败的累计数量。
 */
int testHalfOpenLoopAndEpoch()
{
    // overlap 横跨整个循环区间，两次线性分段都会产生它的交集。
    AudioTimelineTransport transport({
        // at-loop-start 验证包含端点在新 epoch 中立即可见。
        TimelineClipSpec{
            .eventId        = 4U,
            .sourceKey      = "overlap",
            .startFrame     = 5,
            .durationFrames = 20,
            .volume         = 1.0F,
        },
        // cross-loop-end 只输出 [18,20) 的头部，循环后不重复其越界尾部。
        TimelineClipSpec{
            .eventId        = 1U,
            .sourceKey      = "at-loop-start",
            .startFrame     = 10,
            .durationFrames = 2,
            .volume         = 1.0F,
        },
        // at-excluded-end 位于 20，按半开语义不属于循环。
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

    // 空循环必须拒绝且不替换后续有效配置。
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

    // setLoop 时位置仍在零点且未越过 end，因此不会自行递增 epoch。
    // seek 产生 epoch=1；跨界回绕后新分段使用 epoch=2。
    transport.seek(18);
    transport.play();
    std::array<AudioTimelineActiveSpan, 8U> spans;
    const auto result = transport.consumeActiveSpans(6, spans);
    // 四个跨度来自 overlap 尾、cross-loop-end、overlap 头和 loop-start 片段。
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

    // 全量检查排除端点事件，而不是假设它只可能出现在固定数组位置。
    for ( std::size_t index = 0U; index < result.writtenSpanCount; ++index ) {
        failures += expectTrue(spans[index].eventId != 3U,
                               "event at the excluded loop end is not emitted");
    }

    // 再次 seek 开启 epoch=3，精确两帧消费后的立即回绕开启 epoch=4。
    transport.seek(18);
    const auto exactBoundaryResult = transport.consumeActiveSpans(2, spans);
    failures += expectTrue(
        exactBoundaryResult.totalSpanCount == 2U &&
            transport.positionFrame() == 10 && transport.epoch() == 4U,
        "ending exactly at loop end wraps immediately into a new epoch");
    return failures;
}

/**
 * @brief 验证小输出缓冲区仍报告完整数量并确定性推进传输。
 *
 * 两个完全重叠片段需要两个跨度，但输出数组容量只有一项。查询和消费都应
 * 写入一项、报告总数二并设置 truncated。消费仍推进完整十帧，不能因为
 * 调用方容量不足而让音频时间线变慢或重复消费同一区间。
 *
 * @return 容量统计或确定性推进断言失败的累计数量。
 */
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

    // 一项固定数组主动触发无分配截断路径。
    std::array<AudioTimelineActiveSpan, 1U> spans;
    const auto queryResult = transport.queryActiveSpans(0, 10, spans);
    // query 不推进 transport，随后 play 仍从时间线零点消费同一区间。
    int failures = expectTrue(queryResult.writtenSpanCount == 1U &&
                                  queryResult.totalSpanCount == 2U &&
                                  queryResult.truncated,
                              "query reports truncation without allocating");

    // consume 与只读 query 使用相同统计语义，但额外推进位置。
    transport.play();
    const auto consumeResult = transport.consumeActiveSpans(10, spans);
    failures += expectTrue(
        consumeResult.writtenSpanCount == 1U &&
            consumeResult.totalSpanCount == 2U && consumeResult.truncated &&
            transport.positionFrame() == 10,
        "truncated consume still advances the full requested range");
    return failures;
}

/**
 * @brief 验证超长早期片段不会迫使晚期查询扫描全部历史短片段。
 *
 * 一条长片段覆盖到 200000，随后创建十万条只持续一帧的历史短片段。查询
 * [150000,150064) 时只有长片段仍活跃。正确的区间树使用子树最大结束帧
 * 剪枝，访问节点数应少于 128，而不是线性扫描十万条过期记录。
 *
 * visitedNodeCount 是结构性性能证据，不依赖墙钟时间，避免测试受机器负载
 * 和编译模式影响。
 *
 * @return 查询结果或区间树访问上界断言失败的累计数量。
 */
int testSparseLateQueryUsesIntervalIndex()
{
    constexpr std::size_t         SHORT_CLIP_COUNT = 100000U;
    std::vector<TimelineClipSpec> clips;
    clips.reserve(SHORT_CLIP_COUNT + 1U);
    // 长片段起点最早但结束最晚，要求索引同时考虑起止两个维度。
    clips.push_back(TimelineClipSpec{
        .eventId        = 1U,
        .sourceKey      = "long",
        .startFrame     = 0,
        .durationFrames = 200000,
        .volume         = 1.0F,
    });
    // 短片段覆盖早期密集历史，晚期查询时均应由子树最大结束帧整体排除。
    for ( std::size_t index = 0U; index < SHORT_CLIP_COUNT; ++index ) {
        clips.push_back(TimelineClipSpec{
            .eventId        = index + 2U,
            .sourceKey      = "short",
            .startFrame     = static_cast<std::int64_t>(index),
            .durationFrames = 1,
            .volume         = 1.0F,
        });
    }

    AudioTimelineTransport transport(std::move(clips));
    // 两项输出容量足够容纳唯一长片段，确保性能断言不受截断影响。
    std::array<AudioTimelineActiveSpan, 2> spans;
    // 查询位置晚于所有短片段，但仍处于长片段内部。
    const auto result   = transport.queryActiveSpans(150000, 64, spans);
    int        failures = expectTrue(
        result.writtenSpanCount == 1U && result.totalSpanCount == 1U &&
            spans.front().eventId == 1U,
        "late query returns only the still-active long clip");
    failures += expectTrue(
        result.visitedNodeCount < 128U,
        "interval index skips one hundred thousand expired short clips");
    return failures;
}

}  // namespace

/// @brief 运行纯逻辑音频时间线传输测试。
/// @return 所有断言通过时返回 0。
int main()
{
    // 该测试只依赖纯逻辑模块，日志初始化不创建音频设备。
    XLogger::init("AudioTimelineTransportTest");

    // 各场景累计失败，避免首个错误遮蔽后续独立契约。
    int failures = 0;
    failures += testSortedScheduleAndNegativeStart();
    failures += testPlaybackStateSeekAndEpoch();
    failures += testHalfOpenLoopAndEpoch();
    failures += testTruncatedOutputBuffer();
    failures += testSparseLateQueryUsesIntervalIndex();

    // 所有测试函数都已执行后再统一输出摘要，便于一次定位多处失败。
    // 退出码供 CTest 和直接运行统一判断，不以日志级别替代结果。
    if ( failures != 0 ) {
        XERROR("AudioTimelineTransportTest failed with {} assertion(s)",
               failures);
        return 1;
    }
    XINFO("AudioTimelineTransportTest passed");
    return 0;
}
