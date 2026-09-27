#include "audio/AudioTimelineTransport.h"

#include <algorithm>
#include <limits>
#include <utility>

/**
 * @file AudioTimelineTransport.cpp
 * @brief 实现不可变片段调度表上的无分配时间线查询与循环推进。
 *
 * 构造阶段负责清理、排序和建立区间树；播放阶段只读取这些不可变容器，
 * 并把相交片段写入调用方缓冲区。该职责划分保证音频回调不会因查询片段
 * 而分配内存、排序或访问文件系统。
 *
 * 所有区间使用半开语义 [start, end)：
 *
 * - 片段结束帧不再属于片段；
 * - 查询结束帧不产生输出；
 * - 循环结束帧会立即回到循环起点；
 * - 相邻区间不会重复生成边界采样。
 *
 * epoch 标记时间线连续性。显式 seek、stop 和循环回绕会开启新纪元，
 * 使混音端能够丢弃上一段连续播放留下的插值或缓存状态。普通播放、暂停
 * 以及没有改变当前位置的循环配置不会无条件递增纪元。
 *
 * 区间树按已排序片段索引构建，每个节点记录子树最大结束帧。查询先用
 * startFrame 的排序上界排除未来片段，再用最大结束帧排除已整体过期的
 * 子树，从而避免晚期查询线性扫描所有历史短片段。
 *
 * 查询结果中的 sourceKey 是对不可变 m_clips 字符串的借用，生命周期不超过
 * transport；outputStartFrame 相对本次调用的输出缓冲区，sourceStartFrame
 * 相对原片段起点。循环跨界时同一 clip 可以产生多个 span，但各自携带对应
 * epoch，混音端不能只按 eventId 把它们重新合并。
 *
 * 缓冲区截断只限制写入数量。算法仍遍历所有相交叶节点并累计 totalSpanCount，
 * consume 也照常推进完整 frameCount。这样调用方容量不足不会改变播放速度，
 * 同一输入状态在不同缓冲容量下仍得到相同的最终位置和 epoch。
 *
 * @par 构造与热路径边界
 *
 * m_clipEndFrames 和 m_intervalMaxEndTree 只在构造函数写入。播放控制只修改
 * state、position、epoch 与 loopRange；查询递归不会改变索引。若未来需要
 * 动态增删片段，应在外部构造新的 transport 并切换快照，不能让音频回调
 * 同时观察正在重排或扩容的容器。
 *
 * @par 溢出与负时间
 *
 * 时间线允许负起点，以表示时间零前已经开始的资源。结束帧和消费位置使用
 * 饱和加法，避免 int64 上界附近的未定义溢出；循环边界距离则在已证明大小
 * 关系后用 uint64 计算。输出偏移只由查询交集差值得到，始终保持非负。
 *
 * @par 输出顺序
 *
 * 区间树递归始终先左后右，叶节点顺序等同于构造后的 startFrame/eventId
 * 排序。调用方可依赖确定顺序进行可复现混音，但不能假设相同 eventId 只会
 * 出现一次，因为循环跨界和重叠查询都可能产生多个跨度。
 */

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
    // 调用方只使用非负帧数；零或负值保持原位置，避免无意义的边界运算。
    if ( positiveDelta <= 0 ) return frame;

    // 先比较剩余空间再相加，避免有符号溢出本身触发未定义行为。
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
    // 半开边界外没有可消费帧，循环推进会在外层立即执行回绕。
    if ( requestedFrames <= 0 || currentFrame >= boundaryFrame ) return 0;

    // 转为无符号后相减可安全覆盖负起点到正边界的完整距离。
    // 前置条件 currentFrame < boundaryFrame 保证数学结果非负。
    const auto unsignedDistance = static_cast<std::uint64_t>(boundaryFrame) -
                                  static_cast<std::uint64_t>(currentFrame);
    // 请求未越过边界时保留原帧数，避免窄化转换巨大距离。
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
    // 循环身份只由两个半开端点决定，不包含当前播放位置。
    return lhs.startFrame == rhs.startFrame && lhs.endFrame == rhs.endFrame;
}

}  // namespace

AudioTimelineTransport::AudioTimelineTransport(
    std::vector<TimelineClipSpec> clips)
{
    // 非正持续时间没有可与任何半开查询相交的帧，构造时永久移除。
    std::erase_if(clips, [](const TimelineClipSpec& clip) {
        return clip.durationFrames <= 0;
    });
    // startFrame 是区间查询的主序；同起点再按稳定事件 ID 确定输出顺序。
    // stable_sort 还让完全相同键的来源顺序保持确定，便于上层复现混音结果。
    std::stable_sort(
        clips.begin(),
        clips.end(),
        [](const TimelineClipSpec& lhs, const TimelineClipSpec& rhs) {
            if ( lhs.startFrame != rhs.startFrame ) {
                return lhs.startFrame < rhs.startFrame;
            }
            return lhs.eventId < rhs.eventId;
        });

    // 从此以后调度表不再变化，span 中的 sourceKey 视图才能稳定借用字符串。
    m_clips = std::move(clips);
    m_clipEndFrames.reserve(m_clips.size());

    // 结束帧单独缓存，避免热路径反复计算并统一采用饱和语义。
    for ( const auto& clip : m_clips ) {
        const auto endFrame =
            saturatingAdd(clip.startFrame, clip.durationFrames);
        m_clipEndFrames.push_back(endFrame);
    }
    // 四倍容量是数组式二叉区间树的安全上界；空调度表不建立根节点。
    if ( !m_clips.empty() ) {
        m_intervalMaxEndTree.resize(m_clips.size() * 4U);
        static_cast<void>(buildIntervalMaxEndTree(1U, 0U, m_clips.size()));
    }
}

std::span<const TimelineClipSpec> AudioTimelineTransport::clips() const noexcept
{
    // 返回只读借用视图，不复制包含字符串的片段描述。
    return m_clips;
}

AudioTimelinePlaybackState AudioTimelineTransport::state() const noexcept
{
    // 状态是单线程传输控制值；本类不在内部增加原子或锁。
    return m_state;
}

AudioTimelineFrame AudioTimelineTransport::positionFrame() const noexcept
{
    // 返回下一待消费帧，不是上一批输出的最后包含帧。
    return m_positionFrame;
}

std::uint64_t AudioTimelineTransport::epoch() const noexcept
{
    // epoch 只表达连续段身份，不等同于 seek 次数或循环次数。
    return m_epoch;
}

const std::optional<AudioTimelineLoopRange>&
AudioTimelineTransport::loopRange() const noexcept
{
    // optional 引用避免复制，同时由 transport 生命周期约束调用方借用。
    return m_loopRange;
}

void AudioTimelineTransport::play() noexcept
{
    // play 只改变传输状态，当前位置和连续纪元保持不变。
    m_state = AudioTimelinePlaybackState::Playing;
}

void AudioTimelineTransport::pause() noexcept
{
    // 对 Stopped 或已 Paused 再次暂停是幂等操作。
    if ( m_state == AudioTimelinePlaybackState::Playing ) {
        m_state = AudioTimelinePlaybackState::Paused;
    }
}

void AudioTimelineTransport::stop() noexcept
{
    // stop 建立新的时间线连续段，即使此前已经处于零点也递增 epoch。
    m_state         = AudioTimelinePlaybackState::Stopped;
    m_positionFrame = 0;
    ++m_epoch;
}

void AudioTimelineTransport::seek(AudioTimelineFrame frame) noexcept
{
    // 循环只约束排除端点及其以后的位置；循环起点之前仍允许显式定位。
    if ( m_loopRange && frame >= m_loopRange->endFrame ) {
        m_positionFrame = m_loopRange->startFrame;
    } else {
        m_positionFrame = frame;
    }
    // 每次显式 seek 都要求混音端重建连续状态，包括重复 seek 到同一位置。
    ++m_epoch;
}

bool AudioTimelineTransport::setLoop(AudioTimelineLoopRange range) noexcept
{
    // 空区间和反向区间不能形成可推进的循环，保持原配置不变。
    if ( range.startFrame >= range.endFrame ) return false;

    // 仅当范围身份变化且当前位置需要回绕时，配置动作才打断连续纪元。
    const bool rangeChanged =
        !m_loopRange || !loopRangesEqual(*m_loopRange, range);
    // 先保存新范围，使随后的归位使用本次有效起点。
    m_loopRange = range;

    // 位于排除端点或更后方时立即归位，避免下一次消费产生零长度分段。
    if ( m_positionFrame >= range.endFrame ) {
        m_positionFrame = range.startFrame;
        if ( rangeChanged ) ++m_epoch;
    }
    return true;
}

void AudioTimelineTransport::clearLoop() noexcept
{
    // 关闭循环不改变当前线性位置，也不要求混音端重建已连续的源状态。
    m_loopRange.reset();
}

AudioTimelineSpanQueryResult AudioTimelineTransport::queryActiveSpans(
    AudioTimelineFrame startFrame, AudioTimelineFrame frameCount,
    std::span<AudioTimelineActiveSpan> output) const noexcept
{
    // 只读查询使用当前 epoch 标记输出，但不改变播放状态或位置。
    AudioTimelineSpanQueryResult result;
    appendActiveSpans(startFrame, frameCount, 0, m_epoch, output, result);
    // 即使缓冲区不足，递归仍统计全部交集，使调用方可以诊断容量需求。
    result.truncated = result.writtenSpanCount < result.totalSpanCount;
    return result;
}

AudioTimelineSpanQueryResult AudioTimelineTransport::consumeActiveSpans(
    AudioTimelineFrame                 frameCount,
    std::span<AudioTimelineActiveSpan> output) noexcept
{
    AudioTimelineSpanQueryResult result;
    // 暂停和停止状态既不查询片段也不推进位置，维持确定性冻结语义。
    if ( frameCount <= 0 || m_state != AudioTimelinePlaybackState::Playing ) {
        return result;
    }

    // 一次输出可能跨越循环端点，因此分成若干连续线性查询区间。
    AudioTimelineFrame remainingFrames  = frameCount;
    AudioTimelineFrame outputStartFrame = 0;

    while ( remainingFrames > 0 ) {
        // 处理调用前已位于循环外的状态，例如配置循环或外部恢复位置之后。
        if ( m_loopRange && m_positionFrame >= m_loopRange->endFrame ) {
            m_positionFrame = m_loopRange->startFrame;
            ++m_epoch;
        }

        // 当前分段最多延伸到循环排除端点，不能跨边界共享同一个 epoch。
        AudioTimelineFrame segmentFrameCount = remainingFrames;
        if ( m_loopRange && m_positionFrame < m_loopRange->endFrame ) {
            segmentFrameCount = framesBeforeBoundary(
                m_positionFrame, m_loopRange->endFrame, segmentFrameCount);
        }

        // outputStartFrame 把循环后的新线性段放回本次调用的连续输出坐标。
        appendActiveSpans(m_positionFrame,
                          segmentFrameCount,
                          outputStartFrame,
                          m_epoch,
                          output,
                          result);

        // 无论输出缓冲区是否截断，传输都完整消费请求的时间范围。
        m_positionFrame = saturatingAdd(m_positionFrame, segmentFrameCount);
        remainingFrames -= segmentFrameCount;
        outputStartFrame += segmentFrameCount;

        // 精确抵达排除端点也立即回绕，让调用返回的位置始终可直接继续消费。
        if ( m_loopRange && m_positionFrame == m_loopRange->endFrame ) {
            m_positionFrame = m_loopRange->startFrame;
            ++m_epoch;
        }
    }

    // totalSpanCount 跨所有循环分段累计，截断不影响时间线推进结果。
    result.truncated = result.writtenSpanCount < result.totalSpanCount;
    return result;
}

void AudioTimelineTransport::appendActiveSpans(
    AudioTimelineFrame startFrame, AudioTimelineFrame frameCount,
    AudioTimelineFrame outputStartFrame, std::uint64_t queryEpoch,
    std::span<AudioTimelineActiveSpan> output,
    AudioTimelineSpanQueryResult&      result) const noexcept
{
    // 空调度表和非正查询无需访问区间树根节点。
    if ( frameCount <= 0 || m_clips.empty() ) return;

    // 饱和结束帧维持半开区间，即使请求逼近 int64 最大值也不会溢出。
    const AudioTimelineFrame endFrame = saturatingAdd(startFrame, frameCount);
    if ( endFrame <= startFrame ) return;

    // 排序上界排除 startFrame 不小于 queryEnd 的全部未来片段。
    const auto lastCandidate = std::lower_bound(
        m_clips.begin(),
        m_clips.end(),
        endFrame,
        [](const TimelineClipSpec& clip, AudioTimelineFrame frame) {
            return clip.startFrame < frame;
        });

    // lastIndex 是候选前缀长度，不是最后一个有效数组下标。
    const std::size_t lastIndex =
        static_cast<std::size_t>(std::distance(m_clips.begin(), lastCandidate));
    if ( lastIndex == 0U ) return;
    // 根节点覆盖完整调度表，lastIndex 进一步限制实际候选前缀。
    queryIntervalTree(1U,
                      0U,
                      m_clips.size(),
                      lastIndex,
                      startFrame,
                      endFrame,
                      outputStartFrame,
                      queryEpoch,
                      output,
                      result);
}

AudioTimelineFrame AudioTimelineTransport::buildIntervalMaxEndTree(
    std::size_t node, std::size_t begin, std::size_t end)
{
    // 叶节点与一个片段一一对应，保存其半开结束帧。
    if ( end - begin == 1U ) {
        m_intervalMaxEndTree[node] = m_clipEndFrames[begin];
        return m_intervalMaxEndTree[node];
    }

    // 内部节点取左右子树最大结束位置，供查询整棵剪除过期区间。
    const std::size_t middle = begin + (end - begin) / 2U;
    // 构造仅发生一次，递归分配已在 resize 中完成，内部不再扩容。
    const auto leftMax  = buildIntervalMaxEndTree(node * 2U, begin, middle);
    const auto rightMax = buildIntervalMaxEndTree(node * 2U + 1U, middle, end);
    m_intervalMaxEndTree[node] = std::max(leftMax, rightMax);
    return m_intervalMaxEndTree[node];
}

void AudioTimelineTransport::queryIntervalTree(
    std::size_t node, std::size_t begin, std::size_t end,
    std::size_t lastCandidate, AudioTimelineFrame queryStart,
    AudioTimelineFrame queryEnd, AudioTimelineFrame outputStartFrame,
    std::uint64_t queryEpoch, std::span<AudioTimelineActiveSpan> output,
    AudioTimelineSpanQueryResult& result) const noexcept
{
    // visitedNodeCount 是性能回归证据，不参与调度正确性。
    ++result.visitedNodeCount;
    // 第一项排除未来索引，第二项排除结束帧不晚于查询起点的整棵子树。
    if ( begin >= lastCandidate || m_intervalMaxEndTree[node] <= queryStart ) {
        return;
    }

    // 叶节点计算两个半开区间的真实交集，并转换为输出与源内偏移。
    if ( end - begin == 1U ) {
        const auto&              clip = m_clips[begin];
        const AudioTimelineFrame overlapStart =
            std::max(queryStart, clip.startFrame);
        const AudioTimelineFrame overlapEnd =
            std::min(queryEnd, m_clipEndFrames[begin]);
        // 端点接触不构成有效跨度，避免输出零帧片段。
        if ( overlapStart >= overlapEnd ) return;

        // 缓冲区满后仍继续遍历并累计 totalSpanCount，但不越界写入。
        if ( result.writtenSpanCount < output.size() ) {
            output[result.writtenSpanCount] = {
                .clipIndex = begin,
                .eventId   = clip.eventId,
                .sourceKey = clip.sourceKey,
                // queryStart 对应当前循环分段的输出基准，而非整个调用固定零点。
                .outputStartFrame =
                    outputStartFrame + (overlapStart - queryStart),
                // 负起点片段会自然得到已消耗的正 sourceStartFrame。
                .sourceStartFrame = overlapStart - clip.startFrame,
                .frameCount       = overlapEnd - overlapStart,
                .volume           = clip.volume,
                .epoch            = queryEpoch,
            };
            ++result.writtenSpanCount;
        }
        ++result.totalSpanCount;
        return;
    }

    // 保持左后右遍历，使结果顺序与构造后的片段排序一致且可复现。
    const std::size_t middle = begin + (end - begin) / 2U;
    queryIntervalTree(node * 2U,
                      begin,
                      middle,
                      lastCandidate,
                      queryStart,
                      queryEnd,
                      outputStartFrame,
                      queryEpoch,
                      output,
                      result);
    // 右子树仍可能包含同起点或更晚开始但与查询相交的片段。
    queryIntervalTree(node * 2U + 1U,
                      middle,
                      end,
                      lastCandidate,
                      queryStart,
                      queryEnd,
                      outputStartFrame,
                      queryEpoch,
                      output,
                      result);
}

}  // namespace MMM::Audio
