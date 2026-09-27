#include "audio/AudioTimelineMixerNode.h"

#include "audio/KeySoundControl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ice/config/config.hpp>
#include <ice/manage/AudioTrack.hpp>
#include <limits>
#include <ranges>
#include <span>
#include <utility>

namespace MMM::Audio
{
namespace
{

// 本实现把时间线混音拆成控制面与实时数据面，二者遵循以下约束：
//
// - 控制线程负责构造完整的 ScheduleState，音频线程只在 block 边界接管；
// - ScheduleState 发布后对音频线程只读，只有其中的 Transport 状态会被推进；
// - 播放、Seek 与循环命令分别通过单写序列锁邮箱传递，不共享可变对象；
// - 一个偶数序列号表示 payload 完整，奇数表示控制线程仍在写入；
// - 音频线程不等待写入完成，只忽略本轮不稳定快照并在下个 block 重试；
// - 旧调度由音频线程压入退役栈，再由控制线程销毁，避免回调内释放内存；
// - 所有采样资源在调度构造前准备完成，process 不触发解码、排序或扩容；
// - 循环区间统一采用 [startFrame, endFrame)，endFrame 对应下一轮起点；
// - 自然结束位置同时覆盖谱面声明的结束帧与最后一个有效片段的尾帧；
// - 输出缓冲先清零，再累加当前区间的所有活跃片段，最后统一应用主增益；
// - 流式资源未就绪时返回静音，不允许音频线程等待后台解码；
// - 逻辑线程读取的时钟快照必须来自同一发布代次，不能拼接多个 block 的字段。
// - 查询接口允许呈现尚未被音频线程应用的稳定意图，但不会伪造应用确认；
// - 所有实时循环都由 block 帧数、声道数或预先固定的片段数给出明确上界。
//
// 这些约束共同保证控制操作最多延迟到下一个 block，同时保持音频回调有界。

/// @brief 将 size_t 帧数限制为有符号时间线可表达范围。
/// @param value 外部容器或音频资源报告的无符号帧数。
/// @return 可安全参与有符号时间线运算的帧数。
///
/// 时间线允许负起点，因此统一使用有符号坐标；资源长度超过其正数上限时采用
/// 饱和值，避免直接转换产生实现相关结果。
[[nodiscard]] AudioTimelineFrame frameCountFromSize(std::size_t value) noexcept
{
    constexpr auto MAX_FRAME = static_cast<std::uint64_t>(
        std::numeric_limits<AudioTimelineFrame>::max());
    return static_cast<AudioTimelineFrame>(
        std::min(static_cast<std::uint64_t>(value), MAX_FRAME));
}

/// @brief 对时间线帧执行上界饱和加法。
/// @param frame 片段的有符号起始帧。
/// @param positiveDelta 非负资源长度。
/// @return 可表达范围内的排除结束帧。
///
/// 该运算用于计算片段尾部；饱和比回绕更符合“时间线仍延伸到最远处”的语义。
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
/// @param position 当前时间线位置。
/// @param boundary 循环或自然结束的排除边界。
/// @return position 到 boundary 的非负距离。
///
/// 两个有符号值先转成同宽无符号数相减，使负起点到正边界的跨度也不会触发
/// 有符号溢出；最终结果再次限制到 Transport 可接受的范围。
[[nodiscard]] AudioTimelineFrame framesUntil(
    AudioTimelineFrame position, AudioTimelineFrame boundary) noexcept
{
    if ( position >= boundary ) return 0;
    const auto distance = static_cast<std::uint64_t>(boundary) -
                          static_cast<std::uint64_t>(position);
    constexpr auto MAX_FRAME = static_cast<std::uint64_t>(
        std::numeric_limits<AudioTimelineFrame>::max());
    return static_cast<AudioTimelineFrame>(std::min(distance, MAX_FRAME));
}

/// @brief 判断序列锁版本是否表示一次完成且尚未应用的写入。
/// @param sequence 本次 acquire 读取的邮箱版本。
/// @param appliedSequence 音频线程最后应用的版本。
/// @return 版本为偶数且与已应用版本不同则返回 true。
///
/// 本函数只判断版本外形；调用方读取 payload 后仍必须再次验证版本未变化。
[[nodiscard]] bool hasStableUpdate(std::uint64_t sequence,
                                   std::uint64_t appliedSequence) noexcept
{
    return sequence != appliedSequence && (sequence & 1U) == 0U;
}

/// @brief 在读取 relaxed payload 后验证 seqcount 版本仍未变化。
/// @param sequence 被验证的序列计数器。
/// @param expected 首次 acquire 读取到的偶数版本。
/// @return payload 读取期间没有并发 writer 时返回 true。
///
/// acquire fence 是 ARM 等弱内存序平台所需的读取屏障，阻止前面的 payload
/// 读取跨越末次 relaxed 版本验证；本函数不包含锁、循环或分配。
[[nodiscard]] bool sequenceStillStable(
    const std::atomic<std::uint64_t>& sequence, std::uint64_t expected) noexcept
{
    std::atomic_thread_fence(std::memory_order_acquire);
    return sequence.load(std::memory_order_relaxed) == expected;
}

/// @brief 按 Transport 规则将待应用 Seek 目标限制到循环起点。
/// @param requestedFrame 原始 Seek 目标。
/// @param loopEnabled 当前请求是否启用循环。
/// @param loopStartFrame 循环包含起点。
/// @param loopEndFrame 循环排除终点。
/// @return 启用循环且目标不小于排除终点时返回循环起点，否则返回原目标。
[[nodiscard]] AudioTimelineFrame normalizeRequestedSeekFrame(
    AudioTimelineFrame requestedFrame, bool loopEnabled,
    AudioTimelineFrame loopStartFrame, AudioTimelineFrame loopEndFrame) noexcept
{
    return loopEnabled && requestedFrame >= loopEndFrame ? loopStartFrame
                                                         : requestedFrame;
}

/// @brief 获取可跨线程比较的 steady_clock 纳秒时间戳。
/// @return steady_clock 自身纪元起的纳秒计数。
///
/// 时间戳只用于把控制请求与音频发布锚点排序，不映射到日历时间，因此必须使用
/// 不受系统校时影响的 steady_clock。
[[nodiscard]] std::int64_t steadyNowNanoseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

/// @brief 从 IonCachy 音轨建立可供实时混音读取的稳定资源。
/// @param track 已完成加载或已建立流式缓存的音轨所有者。
/// @return 完整缓存返回稳定声道视图；流式缓存返回经 AudioTrack 读取的包装。
///
/// 完整缓存可以直接借用 PCM span，但必须把音轨所有权保存在包装对象中。
/// 流式缓存的页会被淘汰，不能持久保存 origin 返回的临时地址。
/// @warning 资源准备路径可能查询解码状态并分配视图，禁止从音频回调调用。
std::shared_ptr<const PreparedTimelineAudio> PreparedTimelineAudio::fromTrack(
    std::shared_ptr<ice::AudioTrack> track)
{
    if ( !track ) return {};
    const std::size_t frameCount = track->num_frames();
    if ( frameCount == 0U ) return {};

    // 先尝试取得完整缓存的连续声道视图；成功后混音只需 memcpy。
    std::vector<std::span<const float>> channelViews;
    static_cast<void>(track->origin(channelViews, 0.0, frameCount));
    if ( channelViews.empty() &&
         track->cachingStrategy() == ice::CachingStrategy::STREAMING ) {
        // 流式资源不借用可被淘汰的页，回调通过音轨接口读取。
        return std::make_shared<const PreparedTimelineAudio>(
            std::move(track), std::move(channelViews));
    }
    if ( channelViews.empty() ) return {};

    // 所有声道统一裁到最短长度，保证后续按同一帧区间读取不会越界。
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
    // 离线 DSP 可能产生长度略有差异的声道，实时侧只暴露公共有效区间。
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

/// @brief 构造借用 AudioTrack 缓存的只读时间线资源。
/// @param track 保证借用 PCM 或流式读取接口存活的所有者。
/// @param channelViews 完整缓存模式下的稳定声道视图。
///
/// 视图为空不一定代表无效：流式模式只记录总帧数，真正读取委托给 AudioTrack。
PreparedTimelineAudio::PreparedTimelineAudio(
    std::shared_ptr<ice::AudioTrack>    track,
    std::vector<std::span<const float>> channelViews)
    : m_sourceOwner(std::move(track))
    , m_channelViews(std::move(channelViews))
    , m_frameCount(m_channelViews.empty()
                       ? (m_sourceOwner ? m_sourceOwner->num_frames() : 0U)
                       : m_channelViews.front().size())
{
}

/// @brief 构造持有离线处理结果的只读时间线资源。
/// @param channels 所有声道已被工厂裁成相同长度的 PCM。
/// @param sourceOwner 可选的原始资源所有者，用于统一资源生命周期。
///
/// channelViews 只在 ownedChannels 完成移动后建立，因而指向对象最终存储位置。
PreparedTimelineAudio::PreparedTimelineAudio(
    std::vector<std::vector<float>>  channels,
    std::shared_ptr<ice::AudioTrack> sourceOwner)
    : m_sourceOwner(std::move(sourceOwner))
    , m_ownedChannels(std::move(channels))
    , m_frameCount(m_ownedChannels.empty() ? 0U
                                           : m_ownedChannels.front().size())
{
    // 预先固定 span，避免每个音频 block 临时创建声道索引结构。
    m_channelViews.reserve(m_ownedChannels.size());
    for ( const auto& channel : m_ownedChannels ) {
        m_channelViews.emplace_back(channel);
    }
}

/// @brief 返回所有有效声道共有的帧数。
/// @return 构造时固定的资源长度。
std::size_t PreparedTimelineAudio::numFrames() const noexcept
{
    return m_frameCount;
}

/// @brief 返回实时读取应提供的声道数。
/// @return 完整缓存的视图数，流式资源则使用引擎内部格式声道数。
///
/// 流式资源没有长期 channelViews，但 AudioTrack::read 仍按内部格式写入。
std::size_t PreparedTimelineAudio::numChannels() const noexcept
{
    return m_channelViews.empty() && m_sourceOwner
               ? ice::ICEConfig::internal_format.channels
               : m_channelViews.size();
}

/// @brief 查询完整缓存或自有 PCM 的单声道连续视图。
/// @param channelIndex 零基声道索引。
/// @return 越界或流式资源返回空视图。
///
/// 该接口主要供离线验证与测试使用；流式实时读取必须走 read。
std::span<const float> PreparedTimelineAudio::channel(
    std::size_t channelIndex) const noexcept
{
    if ( channelIndex >= m_channelViews.size() ) return {};
    return m_channelViews[channelIndex];
}

/// @brief 把一段已准备 PCM 复制到调用方预分配缓冲区。
/// @param buffer 目标缓冲区，容量由节点构造时固定。
/// @param startFrame 资源内部的零基起始帧。
/// @param frameCount 调用方请求的最大帧数。
/// @return 实际写入帧数；越界、空资源或流式缺页返回零或短读。
///
/// 单声道资源会复制到全部输出声道；多声道资源只复制双方共有的声道。
/// 本函数不负责清空目标剩余区域，调用者必须在读取前清空 scratch。
/// @warning 音频回调热路径；只能执行有界读取与内存复制。
std::size_t PreparedTimelineAudio::read(ice::AudioBuffer& buffer,
                                        std::size_t       startFrame,
                                        std::size_t frameCount) const noexcept
{
    if ( m_channelViews.empty() && m_sourceOwner ) {
        // 流式读取只复制已就绪页，缺页静音，不在回调等待解码线程。
        return m_sourceOwner->read(
            buffer, startFrame, std::min(frameCount, buffer.num_frames()));
    }
    if ( startFrame >= m_frameCount || frameCount == 0U ||
         buffer.raw_ptrs() == nullptr || m_channelViews.empty() ) {
        return 0U;
    }

    // 同时受资源尾部、请求长度与目标容量约束，任何一侧都不得被越过。
    const std::size_t framesToCopy = std::min(
        { frameCount, m_frameCount - startFrame, buffer.num_frames() });
    if ( framesToCopy == 0U ) return 0U;

    const std::size_t outputChannels = buffer.num_channels();
    if ( m_channelViews.size() == 1U && outputChannels > 1U ) {
        // 单声道复制到每个设备声道，保持居中听感而不是只出左声道。
        const float* source = m_channelViews.front().data() + startFrame;
        for ( std::size_t channel = 0U; channel < outputChannels; ++channel ) {
            std::memcpy(buffer.raw_ptrs()[channel],
                        source,
                        framesToCopy * sizeof(float));
        }
        return framesToCopy;
    }

    // 不为缺失声道合成数据；scratch 已清零，因此额外输出声道保持静音。
    const std::size_t channelsToCopy =
        std::min(outputChannels, m_channelViews.size());
    for ( std::size_t channel = 0U; channel < channelsToCopy; ++channel ) {
        std::memcpy(buffer.raw_ptrs()[channel],
                    m_channelViews[channel].data() + startFrame,
                    framesToCopy * sizeof(float));
    }
    return framesToCopy;
}

/// @brief 构造首份不可变调度并发布初始停止快照。
/// @param clips 控制线程已准备的片段资源。
/// @param requestedTimelineEndFrame 谱面声明的排除结束帧。
/// @param maximumProcessFrames 单次实时混合的预分配容量。
/// @param keySoundControls 可选的逐 BGM 轨运行时控制观察者。
///
/// 所有可能分配的工作均在构造期间完成，最终只把裸指针交给音频线程独占。
/// lock-free 静态断言把目标平台不满足实时协议的情况提前变成编译错误。
AudioTimelineMixerNode::AudioTimelineMixerNode(
    std::vector<PreparedTimelineClip> clips,
    AudioTimelineFrame                requestedTimelineEndFrame,
    std::size_t                       maximumProcessFrames,
    const KeySoundControlBank*        keySoundControls)
    : m_keySoundControls(keySoundControls)
{
    // 发布协议依赖这些原子类型不会退化成内部互斥锁。
    static_assert(std::atomic<ScheduleState*>::is_always_lock_free);
    static_assert(std::atomic<AudioTimelineFrame>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    // 首份调度无需经过 pending 邮箱，因为后端尚未开始并发调用 process。
    auto initialState =
        std::make_unique<ScheduleState>(prepareClips(std::move(clips)),
                                        requestedTimelineEndFrame,
                                        maximumProcessFrames,
                                        m_nextScheduleGeneration++);
    m_publishedTimelineEndFrame.store(initialState->timelineEndFrame,
                                      std::memory_order_relaxed);
    m_publishedClipCount.store(initialState->clips.size(),
                               std::memory_order_relaxed);
    m_scheduleState = initialState.release();
    // 构造完成即可让逻辑线程读取一份字段一致的停止状态锚点。
    captureControlEpoch();
    publishTransportSnapshot();
}

/// @brief 在音频后端停止后回收所有调度所有权。
///
/// 析构前置条件是没有并发 process；因此可以直接接管 pending、active 与 retired
/// 三处裸指针。局部 unique_ptr 仅用于确保退出作用域时按普通线程释放资源。
AudioTimelineMixerNode::~AudioTimelineMixerNode()
{
    std::unique_ptr<ScheduleState> pending(
        m_pendingSchedule.exchange(nullptr, std::memory_order_acquire));
    std::unique_ptr<ScheduleState> active(m_scheduleState);
    m_scheduleState = nullptr;
    static_cast<void>(reclaimRetiredSchedules());
}

/// @brief 建立一个可由音频线程独占推进的完整调度状态。
/// @param preparedClips 已过滤并稳定排序的片段表。
/// @param requestedTimelineEndFrame 谱面声明的结束位置。
/// @param requestedMaximumProcessFrames 调用方请求的分段容量。
/// @param scheduleGeneration 节点内单调递增的调度代次。
///
/// scratch 大小在这里按最小一帧固定；即使调用方传零，实时路径也不会出现
/// 零容量导致无法前进的循环。活跃 span 容量等于片段总数，足以容纳最坏交叠。
AudioTimelineMixerNode::ScheduleState::ScheduleState(
    std::vector<PreparedTimelineClip> preparedClips,
    AudioTimelineFrame                requestedTimelineEndFrame,
    std::size_t requestedMaximumProcessFrames, std::uint64_t scheduleGeneration)
    : clips(std::move(preparedClips))
    , transport(AudioTimelineMixerNode::buildClipSpecs(clips))
    , generation(scheduleGeneration)
    , timelineEndFrame(AudioTimelineMixerNode::calculateTimelineEndFrame(
          clips, requestedTimelineEndFrame))
    , maximumProcessFrames(
          std::max<std::size_t>(requestedMaximumProcessFrames, 1U))
    , sourceScratch(ice::ICEConfig::internal_format, maximumProcessFrames)
    , activeSpanScratch(clips.size())
{
}

/// @brief 在控制线程构造并发布一份完整的新时间线调度。
/// @param clips 新调度使用的预备片段。
/// @param requestedTimelineEndFrame 新谱面声明的排除结束帧。
/// @param maximumProcessFrames 新调度的分段 scratch 容量。
/// @return 可与时钟快照对照的单调调度代次。
///
/// 若同一 block 前连续发布多次，pending 槽只保留最后一份，音频线程不会观察
/// 中间调度。当前正在播放的调度保持所有权，直至 block 边界完成切换。
/// @warning 低频控制路径；允许分配和排序，只允许单个控制线程调用。
std::uint64_t AudioTimelineMixerNode::replaceSchedule(
    std::vector<PreparedTimelineClip> clips,
    AudioTimelineFrame                requestedTimelineEndFrame,
    std::size_t                       maximumProcessFrames)
{
    // 控制线程顺手释放上一轮已退役状态，避免频繁替换时长期积压 PCM。
    static_cast<void>(reclaimRetiredSchedules());
    // generation 在构造新状态前分配，供逻辑线程辨认快照属于哪份谱面数据。
    const auto generation = m_nextScheduleGeneration++;
    auto       replacement =
        std::make_unique<ScheduleState>(prepareClips(std::move(clips)),
                                        requestedTimelineEndFrame,
                                        maximumProcessFrames,
                                        generation);
    // 查询接口立即反映最近发布的元数据，不必等待音频设备产生下一个 block。
    m_publishedTimelineEndFrame.store(replacement->timelineEndFrame,
                                      std::memory_order_relaxed);
    m_publishedClipCount.store(replacement->clips.size(),
                               std::memory_order_relaxed);

    // pending 槽只保留最后一次提交；尚未被音频线程看见的旧提交可在此安全析构。
    ScheduleState*                 rawReplacement = replacement.release();
    std::unique_ptr<ScheduleState> superseded(
        m_pendingSchedule.exchange(rawReplacement, std::memory_order_acq_rel));
    return generation;
}

/// @brief 在控制线程回收音频线程已经停止访问的调度链。
/// @return 本轮从退役栈接管并析构的状态数量。
///
/// acquire exchange 同时清空栈头并取得完整链；之后新退役的节点会进入新链，
/// 不会与本轮遍历交错。只有控制线程执行析构，实时线程始终不承担释放成本。
/// @warning 只允许单个非实时控制线程调用。
std::size_t AudioTimelineMixerNode::reclaimRetiredSchedules() noexcept
{
    if ( m_retiredSchedules.load(std::memory_order_acquire) == nullptr ) {
        return 0U;
    }
    // 一次性摘下当前链，缩短与音频线程 CAS 竞争的时间。
    ScheduleState* retired =
        m_retiredSchedules.exchange(nullptr, std::memory_order_acquire);
    std::size_t reclaimedCount = 0U;
    while ( retired ) {
        ScheduleState* next = retired->nextRetired;
        // unique_ptr 在本次循环末尾释放当前节点及其 PCM 所有权。
        std::unique_ptr<ScheduleState> owner(retired);
        retired = next;
        ++reclaimedCount;
    }
    return reclaimedCount;
}

/// @brief 配置自然结束时的实时安全通知入口。
/// @param context 原样传回 listener 的非拥有指针。
/// @param listener 只能执行无阻塞、无分配操作的函数。
///
/// 空 listener 同时清空 context，避免保留一个永远不会再使用的悬空观察指针。
void AudioTimelineMixerNode::setFinalInputListener(
    void* context, FinalInputListener listener) noexcept
{
    m_finalInputListenerContext = listener ? context : nullptr;
    m_finalInputListener        = listener;
}

/// @brief 发布继续播放请求并清除上一轮自然结束标志。
///
/// 实际 Transport 只在 block 边界改变；查询接口通过请求邮箱立即呈现播放意图。
void AudioTimelineMixerNode::play() noexcept
{
    m_publishedFinished.store(false, std::memory_order_relaxed);
    requestPlaybackCommand(PlaybackCommand::Play);
}

/// @brief 发布暂停请求，保留当前位置与循环配置。
///
/// 暂停不改写 Seek 邮箱，恢复播放后从音频线程最后发布的位置继续。
void AudioTimelineMixerNode::pause() noexcept
{
    requestPlaybackCommand(PlaybackCommand::Pause);
}

/// @brief 原子发布“回零并停止”的组合控制意图。
///
/// Seek 邮箱先写零，再发布 Stop 邮箱。音频线程按循环、Seek、播放命令的固定
/// 顺序接收，因此即使两个序列来自相邻写入，也会在同一 block 得到回零状态。
void AudioTimelineMixerNode::stop() noexcept
{
    // 奇数版本封住 payload，偶数版本才允许音频线程应用整条 Seek 命令。
    m_seekSequence.fetch_add(1U, std::memory_order_acq_rel);
    m_requestedSeekFrame.store(0, std::memory_order_relaxed);
    m_requestedSeekSteadyTimeNanoseconds.store(steadyNowNanoseconds(),
                                               std::memory_order_relaxed);
    m_seekSequence.fetch_add(1U, std::memory_order_release);
    requestPlaybackCommand(PlaybackCommand::Stop);
}

/// @brief 发布一个可在暂停状态立即显示的有符号 Seek 目标。
/// @param frame 目标时间线帧，允许位于零点之前。
///
/// 自然结束后的 Seek 把逻辑意图改为暂停，防止旧 Play 请求让新位置未经用户
/// 确认便继续播放。真正的循环归一化在稳定读取循环邮箱后完成。
void AudioTimelineMixerNode::seek(AudioTimelineFrame frame) noexcept
{
    if ( m_publishedFinished.exchange(false, std::memory_order_relaxed) ) {
        requestPlaybackCommand(PlaybackCommand::Pause);
    }
    // 时间戳与帧坐标属于同一 seqcount 事务，快照不得混用两次 Seek 的字段。
    m_seekSequence.fetch_add(1U, std::memory_order_acq_rel);
    m_requestedSeekFrame.store(frame, std::memory_order_relaxed);
    m_requestedSeekSteadyTimeNanoseconds.store(steadyNowNanoseconds(),
                                               std::memory_order_relaxed);
    m_seekSequence.fetch_add(1U, std::memory_order_release);
}

/// @brief 发布新的半开循环区间。
/// @param range 包含起点、排除终点的时间线范围。
/// @return 区间非空时返回 true；非法请求不改变现有邮箱。
///
/// 三个 payload 字段由同一序列号保护，避免读者观察到新起点配旧终点。
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

/// @brief 发布关闭循环的请求并保留旧端点供诊断。
///
/// 关闭只需原子改变 enabled；端点在禁用状态没有语义，不必额外写入和竞争。
void AudioTimelineMixerNode::clearLoop() noexcept
{
    m_loopSequence.fetch_add(1U, std::memory_order_acq_rel);
    m_requestedLoopEnabled.store(false, std::memory_order_relaxed);
    m_loopSequence.fetch_add(1U, std::memory_order_release);
}

/// @brief 返回逻辑界面此刻应显示的时间线位置。
/// @return 未应用的稳定 Seek 优先，其次 Stop 零点，最后是已发布音频位置。
///
/// 该查询不等待音频设备产生 block，使暂停拖动与停止反馈能够立即更新。
/// 如果 Seek 或循环邮箱恰逢写入，则退回最近完整发布位置而不是自旋。
AudioTimelineFrame AudioTimelineMixerNode::positionFrame() const noexcept
{
    // 先尝试读取未消费 Seek；已应用版本必须以快照发布点为准。
    const auto seekSequence = m_seekSequence.load(std::memory_order_acquire);
    if ( (seekSequence & 1U) == 0U &&
         seekSequence !=
             m_publishedAppliedSeekSequence.load(std::memory_order_acquire) ) {
        const auto requestedFrame =
            m_requestedSeekFrame.load(std::memory_order_relaxed);
        if ( sequenceStillStable(m_seekSequence, seekSequence) ) {
            bool               loopEnabled = false;
            AudioTimelineFrame loopStart   = 0;
            AudioTimelineFrame loopEnd     = 0;
            // Seek 到循环排除终点按 Transport 规则折回包含起点。
            if ( tryReadRequestedLoop(loopEnabled, loopStart, loopEnd) ) {
                return normalizeRequestedSeekFrame(
                    requestedFrame, loopEnabled, loopStart, loopEnd);
            }
        }
    }
    if ( m_requestedPlaybackCommand.load(std::memory_order_relaxed) ==
         PlaybackCommand::Stop ) {
        return 0;
    }
    return m_publishedPositionFrame.load(std::memory_order_relaxed);
}

/// @brief 合并最近音频发布锚点与尚未应用的控制请求。
/// @return 字段来自稳定版本时返回 valid 快照，竞争超过上限则返回无效快照。
///
/// 第一层序列锁保证位置、状态、纪元、调度代次与发布时间属于同一 block。
/// 第二层分别覆盖 pending Seek 和播放命令，让逻辑线程看见即时意图。
/// 读取最多尝试四次，防止音频线程高频发布时让 UI 线程无界自旋。
///
/// 快照字段的解释规则如下：
///
/// - positionFrame 是下一段音频的起点，不是刚播放完成的最后一帧；
/// - steadyTimeNanoseconds 是该位置或最近控制请求成为有效锚点的时刻；
/// - epoch 表示最终 Transport 连续性，controlEpoch 表示应用控制后的连续性；
/// - 两个纪元不同说明同一 block 内发生了循环跳转；
/// - scheduleGeneration 区分相同位置但来自不同调度数据的锚点；
/// - appliedSeekSequence 与 seekSequence 不同表示界面正在展示待应用 Seek；
/// - appliedPlaybackSequence 与 playbackSequence 不同表示状态来自即时控制意图；
/// - finished 只描述自然结束，不等同于普通 Pause 或显式 Stop；
/// - valid=false 是正常竞争结果，调用方应继续使用上一份有效锚点推演。
/// @warning 逻辑热路径；只能进行有界原子读取，不得加入锁或等待。
AudioTimelineClockSnapshot
AudioTimelineMixerNode::clockSnapshot() const noexcept
{
    constexpr std::size_t      MAX_READ_ATTEMPTS = 4U;
    AudioTimelineClockSnapshot snapshot;

    // 发布序列为奇数时音频线程正在改写 payload，本轮直接重试。
    for ( std::size_t attempt = 0U; attempt < MAX_READ_ATTEMPTS; ++attempt ) {
        const auto sequence =
            m_clockSnapshotSequence.load(std::memory_order_acquire);
        if ( (sequence & 1U) != 0U ) continue;

        // payload 使用 relaxed 读取，首尾 acquire 验证负责建立一致性边界。
        snapshot.positionFrame =
            m_publishedPositionFrame.load(std::memory_order_relaxed);
        snapshot.steadyTimeNanoseconds =
            m_publishedSteadyTimeNanoseconds.load(std::memory_order_relaxed);
        snapshot.state = m_publishedState.load(std::memory_order_relaxed);
        snapshot.epoch = m_publishedEpoch.load(std::memory_order_relaxed);
        snapshot.controlEpoch =
            m_publishedControlEpoch.load(std::memory_order_relaxed);
        snapshot.scheduleGeneration =
            m_publishedScheduleGeneration.load(std::memory_order_relaxed);
        snapshot.finished =
            m_clockPublishedFinished.load(std::memory_order_relaxed);
        snapshot.appliedSeekSequence =
            m_publishedAppliedSeekSequence.load(std::memory_order_relaxed);
        snapshot.appliedPlaybackSequence =
            m_publishedAppliedPlaybackCommandSequence.load(
                std::memory_order_relaxed);

        // 任何字段读取期间发生发布都使整份候选快照失效。
        if ( !sequenceStillStable(m_clockSnapshotSequence, sequence) ) {
            continue;
        }

        snapshot.sequence = sequence;
        snapshot.valid    = true;

        // 尚未进入音频 block 的 Seek 覆盖位置与时间戳，但不伪造 Transport
        // 纪元。
        const auto seekSequence =
            m_seekSequence.load(std::memory_order_acquire);
        if ( (seekSequence & 1U) != 0U ) return {};
        const auto requestedFrame =
            m_requestedSeekFrame.load(std::memory_order_relaxed);
        const auto requestedSeekTime =
            m_requestedSeekSteadyTimeNanoseconds.load(
                std::memory_order_relaxed);
        if ( !sequenceStillStable(m_seekSequence, seekSequence) ) {
            return {};
        }
        snapshot.seekSequence = seekSequence;
        if ( seekSequence != snapshot.appliedSeekSequence ) {
            bool               loopEnabled = false;
            AudioTimelineFrame loopStart   = 0;
            AudioTimelineFrame loopEnd     = 0;
            if ( !tryReadRequestedLoop(loopEnabled, loopStart, loopEnd) ) {
                return {};
            }
            snapshot.positionFrame = normalizeRequestedSeekFrame(
                requestedFrame, loopEnabled, loopStart, loopEnd);
            snapshot.steadyTimeNanoseconds = requestedSeekTime;
            snapshot.finished              = false;
        }

        // 播放命令最后叠加，因为 Stop/Play 可能进一步规范显示位置和 finished。
        const auto playbackSequence =
            m_playbackCommandSequence.load(std::memory_order_acquire);
        if ( (playbackSequence & 1U) != 0U ) return {};
        const auto command =
            m_requestedPlaybackCommand.load(std::memory_order_relaxed);
        const auto requestedPlaybackTime =
            m_requestedPlaybackSteadyTimeNanoseconds.load(
                std::memory_order_relaxed);
        if ( !sequenceStillStable(m_playbackCommandSequence,
                                  playbackSequence) ) {
            return {};
        }
        snapshot.playbackSequence = playbackSequence;
        if ( playbackSequence != snapshot.appliedPlaybackSequence ) {
            snapshot.steadyTimeNanoseconds = requestedPlaybackTime;
            switch ( command ) {
            case PlaybackCommand::Stop:
                snapshot.positionFrame = 0;
                snapshot.state         = AudioTimelinePlaybackState::Stopped;
                snapshot.finished      = false;
                break;
            case PlaybackCommand::Play:
                if ( snapshot.finished ) {
                    snapshot.positionFrame = 0;
                }
                snapshot.state    = AudioTimelinePlaybackState::Playing;
                snapshot.finished = false;
                break;
            case PlaybackCommand::Pause:
                snapshot.state = AudioTimelinePlaybackState::Paused;
                break;
            }
        }
        // 到此三个序列锁均稳定，调用方可以安全用作视觉推演锚点。
        return snapshot;
    }

    return {};
}

/// @brief 返回最近一次 process 开始时固定的时间线位置。
/// @return 当前混音 block 的包含起点。
///
/// 预定音效节点可用该锚点计算 block 内偏移；它不等同于 block 完成后发布的
/// positionFrame，且依赖 MixBus 按固定来源顺序调用节点。
AudioTimelineFrame AudioTimelineMixerNode::blockStartFrame() const noexcept
{
    return m_publishedBlockStartFrame.load(std::memory_order_relaxed);
}

/// @brief 为下游变速器预告下一段连续时间线输入。
/// @param maximumFrames 下游本轮最多希望拉取的输入帧数。
/// @return 不跨循环跳转或自然结束点的区间长度及末端种类。
///
/// 本函数先在 block 边界应用调度和控制，再把所得区间封存给紧随其后的
/// process。这样查询之后到实际拉取之前到达的新 Seek 不会插入已承诺区间。
/// 循环恰好位于排除终点时先回到起点；非循环到末尾时发布一次 final。
/// @warning 音频热路径；调用线程必须与下一次 process 相同。
AudioTimelineInputBoundary AudioTimelineMixerNode::prepareInputBoundary(
    std::size_t maximumFrames) noexcept
{
    // 每次查询覆盖旧承诺，零长度或非播放状态不会封存输入区间。
    m_hasPreparedInputBoundary = false;
    m_preparedInputFrameCount  = 0U;
    applyPendingSchedule();
    applyPendingControls();
    captureControlEpoch();
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
            // 跳转本身不消耗输出帧，重新计算起点到下一边界的可用长度。
            schedule.transport.seek(loopRange->startFrame);
            continue;
        }
        if ( !loopRange && position >= schedule.timelineEndFrame ) {
            // final 可以以零帧边界单独返回，供下游立即刷新尾部缓存。
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
        // frameCount 等于边界距离时，本次拉取末端需要执行跳转或结束提交。
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

/// @brief 返回最近完整音频快照中的播放状态。
/// @return 音频线程已经应用的 Transport 状态。
AudioTimelinePlaybackState AudioTimelineMixerNode::state() const noexcept
{
    return m_publishedState.load(std::memory_order_relaxed);
}

/// @brief 返回控制线程最近发布的即时播放意图。
/// @return 自然结束视为停止，否则映射最新邮箱命令。
///
/// 该值可能领先于 state 一个 block，供按钮与状态栏立即反馈用户操作。
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

/// @brief 返回最近完成 block 的传输纪元。
/// @return Seek、循环跳转等非连续变化累计后的纪元。
std::uint64_t AudioTimelineMixerNode::epoch() const noexcept
{
    return m_publishedEpoch.load(std::memory_order_relaxed);
}

/// @brief 判断非循环时间线是否已经自然结束。
/// @return final 边界已通知且尚未被新 Play 或 Seek 清除时返回 true。
bool AudioTimelineMixerNode::finished() const noexcept
{
    return m_publishedFinished.load(std::memory_order_relaxed);
}

/// @brief 返回最近发布调度的复合排除结束帧。
/// @return 谱面结束与所有有效片段尾帧的最大值。
AudioTimelineFrame AudioTimelineMixerNode::timelineEndFrame() const noexcept
{
    return m_publishedTimelineEndFrame.load(std::memory_order_relaxed);
}

/// @brief 返回最近发布调度中的有效片段数。
/// @return 已移除空音频资源后的片段数量。
std::size_t AudioTimelineMixerNode::clipCount() const noexcept
{
    return m_publishedClipCount.load(std::memory_order_relaxed);
}

/// @brief 更新逐片段混合之后统一应用的线性主增益。
/// @param gain 非有限值或负数归一化为零。
///
/// 单个 relaxed 原子足以表达独立参数的“尽快生效”，无需把它纳入控制序列锁。
void AudioTimelineMixerNode::setMasterGain(float gain) noexcept
{
    m_masterGain.store(std::isfinite(gain) ? std::max(gain, 0.0F) : 0.0F,
                       std::memory_order_relaxed);
}

/// @brief 返回控制线程最近设置的主增益。
/// @return 已完成合法化的非负线性增益。
float AudioTimelineMixerNode::masterGain() const noexcept
{
    return m_masterGain.load(std::memory_order_relaxed);
}

/// @brief 返回最近输出 block 的左声道绝对峰值。
/// @return 已应用主增益的峰值；静音或空 block 为零。
float AudioTimelineMixerNode::leftLevel() const noexcept
{
    return m_leftLevel.load(std::memory_order_relaxed);
}

/// @brief 返回最近输出 block 的右声道绝对峰值。
/// @return 单声道输出复制左峰值，多声道取第二声道峰值。
float AudioTimelineMixerNode::rightLevel() const noexcept
{
    return m_rightLevel.load(std::memory_order_relaxed);
}

/// @brief 清空并生成一个完整设备输出 block。
/// @param buffer 后端预分配且格式固定的输出缓冲区。
///
/// block 可被 maximumProcessFrames、循环排除终点和自然结束位置进一步切段。
/// 每段由 Transport 一次性产生活跃 span，混音后才推进到下一时间线位置。
/// 已由 prepareInputBoundary 封存且帧数吻合的拉取不会再次接收控制命令。
///
/// 处理顺序维持以下不变量：
///
/// - 进入函数立即清空整块，任何提前退出都得到确定静音；
/// - 只有未封存边界的普通拉取才接管新调度和新控制；
/// - blockStartFrame 在任何样本消费前发布；
/// - 非 Playing 状态不推进 Transport，但仍刷新电平与时钟；
/// - 每次 mixSegment 的长度不超过 scratch 容量；
/// - 单段绝不跨循环终点或自然结束位置；
/// - 循环跳转不占用输出帧，同一 block 可继续从循环起点填充；
/// - 非循环尾部之后保持由初始 clear 产生的静音；
/// - final 通知在抵达边界的同一 block 内提交；
/// - 主增益只在所有片段累加完后执行一次；
/// - 最终快照与本 block 实际消费后的 Transport 状态一致。
/// @warning 音频回调热路径；禁止分配、锁、日志、解码和无界遍历。
void AudioTimelineMixerNode::process(ice::AudioBuffer& buffer)
{
    // 输出总是从确定性静音开始，短读、缺声道与未覆盖尾部自然保持为零。
    buffer.clear();
    const bool consumePreparedBoundary =
        m_hasPreparedInputBoundary &&
        m_preparedInputFrameCount == buffer.num_frames();
    m_hasPreparedInputBoundary = false;
    m_preparedInputFrameCount  = 0U;
    if ( !consumePreparedBoundary ) {
        // 普通拉取把调度和控制统一固定在 block 起点。
        applyPendingSchedule();
        applyPendingControls();
        captureControlEpoch();
    }
    m_publishedBlockStartFrame.store(
        m_scheduleState ? m_scheduleState->transport.positionFrame() : 0,
        std::memory_order_relaxed);

    if ( !m_scheduleState ||
         m_scheduleState->transport.state() !=
             AudioTimelinePlaybackState::Playing ||
         buffer.num_frames() == 0U ) {
        // 暂停与停止仍发布零电平和稳定时钟，使控制侧不会保留旧峰值。
        applyMasterGainAndPublishLevels(buffer);
        publishTransportSnapshot();
        return;
    }

    auto&       schedule         = *m_scheduleState;
    std::size_t outputStartFrame = 0U;
    // 循环可能让一个设备 block 包含多个时间线区间，因此逐段填满输出。
    while ( outputStartFrame < buffer.num_frames() ) {
        const auto loopRange = schedule.transport.loopRange();
        const auto position  = schedule.transport.positionFrame();

        if ( !loopRange && position >= schedule.timelineEndFrame ) {
            schedule.transport.pause();
            markFinishedAndNotify();
            break;
        }

        // 先受 scratch 容量约束，再受当前时间线边界约束。
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
            // 零长度循环边界只改变位置；非循环零长度则是最终结束。
            if ( loopRange ) {
                schedule.transport.seek(loopRange->startFrame);
                continue;
            }
            schedule.transport.pause();
            markFinishedAndNotify();
            break;
        }

        // mixSegment 同时消费 Transport，因此返回后 position 已前进
        // frameCount。
        mixSegment(buffer, outputStartFrame, frameCount);
        outputStartFrame += frameCount;
    }

    // 恰好在最后一段末端抵达终点时也要在本 block 内发布 final。
    if ( !schedule.transport.loopRange() &&
         schedule.transport.state() == AudioTimelinePlaybackState::Playing &&
         schedule.transport.positionFrame() >= schedule.timelineEndFrame ) {
        schedule.transport.pause();
        markFinishedAndNotify();
    }

    // 主增益与电平在所有片段完成累加后统一处理，避免交叠片段重复缩放。
    applyMasterGainAndPublishLevels(buffer);
    publishTransportSnapshot();
}

/// @brief 在非实时线程过滤并规范化即将发布的片段表。
/// @param clips 允许包含空资源、任意顺序和异常音量的输入片段。
/// @return 只含有效资源且顺序确定的调度片段。
///
/// 排序结果同时决定 Transport 索引和浮点累加顺序，因此必须在发布前固定。
/// @warning 允许分配和 O(n log n) 排序，只能用于构造或调度替换路径。
std::vector<PreparedTimelineClip> AudioTimelineMixerNode::prepareClips(
    std::vector<PreparedTimelineClip> clips)
{
    // 空资源永远不会产生 span，提前移除可缩小实时扫描和 scratch 容量。
    std::erase_if(clips, [](const PreparedTimelineClip& clip) {
        return !clip.audio || clip.audio->numFrames() == 0U;
    });
    // 负音量按静音处理；非有限输入回退到不改变响度的单位增益。
    for ( auto& clip : clips ) {
        clip.volume =
            std::isfinite(clip.volume) ? std::max(clip.volume, 0.0F) : 1.0F;
    }
    // 先按起点再按事件 ID 建立确定顺序，稳定排序保留完全相同键的输入次序。
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

/// @brief 把持有 PCM 的片段映射成 Transport 所需的轻量调度描述。
/// @param clips 已过滤和排序的不可变片段表。
/// @return 与 clips 索引一一对应的时序描述数组。
///
/// Transport 不接触资源所有权，只负责计算哪个片段在输出区间内活跃。
/// 保持相同索引使实时混音可以由 ActiveSpan 直接定位 PreparedTimelineClip。
std::vector<TimelineClipSpec> AudioTimelineMixerNode::buildClipSpecs(
    const std::vector<PreparedTimelineClip>& clips)
{
    std::vector<TimelineClipSpec> specs;
    // 非实时构造阶段一次性预留，避免逐片段增长产生多次分配。
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

/// @brief 计算调度能够自然播放到的最远排除结束帧。
/// @param clips 已准备的有效资源片段。
/// @param requestedTimelineEndFrame 谱面显式声明的结束位置。
/// @return 不小于零且覆盖所有片段尾部的复合结束帧。
///
/// 负起点片段的尾部仍可能越过零点；超大长度采用饱和加法，不允许回绕缩短。
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

/// @brief 在音频 block 起点接管最后发布的待用调度。
///
/// exchange 确保每个新状态只被一个音频线程接管。旧 active 状态先停止访问，
/// 再压入退役栈；实时线程只转移指针，不执行 PCM 析构。
/// @warning 音频热路径；操作数量与片段数无关。
void AudioTimelineMixerNode::applyPendingSchedule() noexcept
{
    ScheduleState* replacement =
        m_pendingSchedule.exchange(nullptr, std::memory_order_acquire);
    if ( !replacement ) return;

    // 新 Transport 继承最近控制意图，而不是恢复其构造时的默认停止状态。
    ScheduleState* previous = m_scheduleState;
    m_scheduleState         = replacement;
    initializeReplacementTransport(*replacement);
    retireSchedule(previous);
}

/// @brief 将不再被音频线程访问的状态压入无锁退役栈。
/// @param state 已从 active 或 pending 路径摘除的状态。
///
/// CAS 失败只表示控制线程刚摘链或栈头变化，retiredHead 会被原子更新后重试。
/// 节点的 nextRetired 仅在退役后使用，不与实时混音中的只读字段冲突。
/// @warning 音频热路径；不得在这里删除 state。
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

/// @brief 让新调度的 Transport 继承最近稳定的循环、位置与播放意图。
/// @param state 刚由音频线程接管、尚未开始产出样本的状态。
///
/// 应用顺序固定为循环、Seek、播放命令：循环先决定 Seek 是否归一化，播放命令
/// 最后决定终点后的 Play 是否回零。邮箱写入竞争时不等待，下个 block 再补应用。
/// 已自然结束的节点替换数据后仍保持停止，只有显式 Play 或 Seek 才能复活。
/// @warning 音频热路径；只做常数次原子访问和 Transport 状态修改。
void AudioTimelineMixerNode::initializeReplacementTransport(
    ScheduleState& state) noexcept
{
    // 循环命令即使已应用于旧状态，也必须复制到新的 Transport。
    const auto loopSequence = m_loopSequence.load(std::memory_order_acquire);
    if ( (loopSequence & 1U) == 0U ) {
        const bool enabled =
            m_requestedLoopEnabled.load(std::memory_order_relaxed);
        const auto startFrame =
            m_requestedLoopStartFrame.load(std::memory_order_relaxed);
        const auto endFrame =
            m_requestedLoopEndFrame.load(std::memory_order_relaxed);
        if ( sequenceStillStable(m_loopSequence, loopSequence) ) {
            if ( enabled ) {
                static_cast<void>(
                    state.transport.setLoop({ startFrame, endFrame }));
            }
            m_appliedLoopSequence = loopSequence;
        }
    }

    // 默认继承最近完成 block 的位置，未应用 Seek 则覆盖这个候选值。
    AudioTimelineFrame replacementPosition =
        m_publishedPositionFrame.load(std::memory_order_relaxed);
    const auto seekSequence = m_seekSequence.load(std::memory_order_acquire);
    if ( hasStableUpdate(seekSequence, m_appliedSeekSequence) ) {
        const auto requestedPosition =
            m_requestedSeekFrame.load(std::memory_order_relaxed);
        if ( sequenceStillStable(m_seekSequence, seekSequence) ) {
            replacementPosition   = requestedPosition;
            m_appliedSeekSequence = seekSequence;
        }
    }
    state.transport.seek(replacementPosition);

    // 播放邮箱可能没有“新版本”，但新状态仍须按当前完整命令初始化。
    const auto playbackSequence =
        m_playbackCommandSequence.load(std::memory_order_acquire);
    if ( (playbackSequence & 1U) != 0U ) return;
    const auto command =
        m_requestedPlaybackCommand.load(std::memory_order_relaxed);
    if ( !sequenceStillStable(m_playbackCommandSequence, playbackSequence) ) {
        return;
    }

    if ( m_publishedFinished.load(std::memory_order_relaxed) ) {
        // 数据热替换不是用户播放请求，不能隐式重新启动已结束时间线。
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
        // Transport 的 Pause 只对 Playing 有效，先 Play 再 Pause
        // 以保留目标位置。
        state.transport.play();
        state.transport.pause();
        break;
    }
    m_appliedPlaybackCommandSequence = playbackSequence;
}

/// @brief 以幂等方式发布自然结束并通知下游一次。
///
/// 同一终点可能同时被分段循环后的检查与边界查询观察；exchange 避免重复触发
/// final。新 Play 或 Seek 会清除此标志，为下一轮自然结束重新开放通知。
/// @warning listener 在音频线程同步调用，必须满足实时安全约束。
void AudioTimelineMixerNode::markFinishedAndNotify() noexcept
{
    const bool wasFinished =
        m_publishedFinished.exchange(true, std::memory_order_relaxed);
    if ( !wasFinished && m_finalInputListener ) {
        m_finalInputListener(m_finalInputListenerContext);
    }
}

/// @brief 在当前 block 起点应用所有已经稳定发布的控制邮箱。
///
/// 循环先应用，Seek 再改变位置，播放命令最后改变状态。每个邮箱仅读一次候选
/// 版本并验证一次，不稳定写入留到下一 block，绝不等待控制线程。
/// Seek 在请求状态仍为 Play 时会恢复播放，满足暂停设备后拖动再继续的语义。
/// @warning 音频热路径；不得加入日志、锁或无界重试。
void AudioTimelineMixerNode::applyPendingControls() noexcept
{
    if ( !m_scheduleState ) return;
    auto& transport = m_scheduleState->transport;

    // 循环变更可能立即规范当前 Transport 位置并递增其纪元。
    const auto loopSequence = m_loopSequence.load(std::memory_order_acquire);
    if ( hasStableUpdate(loopSequence, m_appliedLoopSequence) ) {
        const bool enabled =
            m_requestedLoopEnabled.load(std::memory_order_relaxed);
        const auto startFrame =
            m_requestedLoopStartFrame.load(std::memory_order_relaxed);
        const auto endFrame =
            m_requestedLoopEndFrame.load(std::memory_order_relaxed);
        if ( sequenceStillStable(m_loopSequence, loopSequence) ) {
            if ( enabled ) {
                static_cast<void>(transport.setLoop({ startFrame, endFrame }));
            } else {
                transport.clearLoop();
            }
            m_appliedLoopSequence = loopSequence;
        }
    }

    // Seek payload 只有在首尾版本相同且为偶数时才可作为完整命令应用。
    const auto seekSequence = m_seekSequence.load(std::memory_order_acquire);
    if ( hasStableUpdate(seekSequence, m_appliedSeekSequence) ) {
        const auto frame = m_requestedSeekFrame.load(std::memory_order_relaxed);
        if ( sequenceStillStable(m_seekSequence, seekSequence) ) {
            transport.seek(frame);
            if ( m_requestedPlaybackCommand.load(std::memory_order_relaxed) ==
                 PlaybackCommand::Play ) {
                transport.play();
            }
            m_appliedSeekSequence = seekSequence;
            m_publishedFinished.store(false, std::memory_order_relaxed);
        }
    }

    // 没有新播放命令时提前返回，前面的循环与 Seek 仍然已经生效。
    const auto playbackSequence =
        m_playbackCommandSequence.load(std::memory_order_acquire);
    if ( !hasStableUpdate(playbackSequence,
                          m_appliedPlaybackCommandSequence) ) {
        return;
    }

    const auto command =
        m_requestedPlaybackCommand.load(std::memory_order_relaxed);
    if ( !sequenceStillStable(m_playbackCommandSequence, playbackSequence) ) {
        return;
    }

    switch ( command ) {
    case PlaybackCommand::Stop:
        transport.stop();
        m_publishedFinished.store(false, std::memory_order_relaxed);
        break;
    case PlaybackCommand::Play:
        // 非循环时间线从自然终点重新播放时按用户预期从零开始。
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

/// @brief 记录本 block 应用控制后的 Transport 纪元。
///
/// 随后若 process 在同一 block 内跨循环边界，最终 epoch 会继续增加；快照中
/// controlEpoch 与 epoch 的差异由下游识别为内容内部跳转，而非新控制命令。
void AudioTimelineMixerNode::captureControlEpoch() noexcept
{
    m_controlEpoch = m_scheduleState ? m_scheduleState->transport.epoch() : 0U;
}

/// @brief 有界读取一份完整循环配置。
/// @param enabled 返回循环开关。
/// @param startFrame 返回包含起点。
/// @param endFrame 返回排除终点。
/// @return 写入期间或读取后版本变化时返回 false。
///
/// 调用方在失败时沿用已发布状态；这里不能循环等待单控制线程完成写入。
bool AudioTimelineMixerNode::tryReadRequestedLoop(
    bool& enabled, AudioTimelineFrame& startFrame,
    AudioTimelineFrame& endFrame) const noexcept
{
    const auto sequence = m_loopSequence.load(std::memory_order_acquire);
    if ( (sequence & 1U) != 0U ) return false;

    enabled    = m_requestedLoopEnabled.load(std::memory_order_relaxed);
    startFrame = m_requestedLoopStartFrame.load(std::memory_order_relaxed);
    endFrame   = m_requestedLoopEndFrame.load(std::memory_order_relaxed);
    return sequenceStillStable(m_loopSequence, sequence);
}

/// @brief 把音频线程当前状态作为一份一致时钟锚点发布给逻辑线程。
///
/// 外层 sequence 先变奇数，再写全部 relaxed payload，最后以 release 变回偶数。
/// 读者只有在首尾观察到同一个偶数版本时才接受字段。无 active 状态时统一发布
/// 零点停止快照，避免保留上一调度的纪元与代次。
/// @warning 音频热路径；字段数量固定，不得扩展为可变容器复制。
void AudioTimelineMixerNode::publishTransportSnapshot() noexcept
{
    const auto steadyTimeNanoseconds = steadyNowNanoseconds();
    // 奇数版本声明写入开始，阻止读者接受半更新 payload。
    m_clockSnapshotSequence.fetch_add(1U, std::memory_order_acq_rel);
    if ( !m_scheduleState ) {
        m_publishedPositionFrame.store(0, std::memory_order_relaxed);
        m_publishedState.store(AudioTimelinePlaybackState::Stopped,
                               std::memory_order_relaxed);
        m_publishedEpoch.store(0U, std::memory_order_relaxed);
        m_publishedControlEpoch.store(0U, std::memory_order_relaxed);
        m_publishedScheduleGeneration.store(0U, std::memory_order_relaxed);
    } else {
        m_publishedPositionFrame.store(
            m_scheduleState->transport.positionFrame(),
            std::memory_order_relaxed);
        m_publishedState.store(m_scheduleState->transport.state(),
                               std::memory_order_relaxed);
        m_publishedEpoch.store(m_scheduleState->transport.epoch(),
                               std::memory_order_relaxed);
        m_publishedControlEpoch.store(m_controlEpoch,
                                      std::memory_order_relaxed);
        m_publishedScheduleGeneration.store(m_scheduleState->generation,
                                            std::memory_order_relaxed);
    }
    // 时间戳与状态一起发布，UI 可从同一锚点平滑推演当前播放位置。
    m_publishedSteadyTimeNanoseconds.store(steadyTimeNanoseconds,
                                           std::memory_order_relaxed);
    m_clockPublishedFinished.store(
        m_publishedFinished.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    m_publishedAppliedSeekSequence.store(m_appliedSeekSequence,
                                         std::memory_order_relaxed);
    m_publishedAppliedPlaybackCommandSequence.store(
        m_appliedPlaybackCommandSequence, std::memory_order_relaxed);
    // release 偶数版本提交上述所有字段。
    m_clockSnapshotSequence.fetch_add(1U, std::memory_order_release);
}

/// @brief 混合一个不跨时间线不连续点的连续输出片段。
/// @param output 完整设备 block 的输出缓冲。
/// @param outputStartFrame 本片段写入设备 block 的起始偏移。
/// @param frameCount 本片段要消费的时间线帧数。
///
/// Transport 返回的 span 已包含源偏移、输出偏移和片段静态音量。运行时 BGM
/// 控制在每个 block 读取，使静音和轨道增益无需重建整份调度即可生效。
/// scratch 在 ScheduleState 构造时固定容量；资源短读后的剩余帧保持静音。
/// 若活跃 span 超出预分配容量，整段保持静音但 Transport 仍前进，避免回调卡死。
///
/// 声道映射保持简单且可预测：PreparedTimelineAudio::read 已把单声道扩展到全部
/// 输出声道，多声道只复制共有声道；本层不会在实时线程执行重采样或矩阵混音。
/// 每个 span 的 effectiveVolume 是静态事件音量与当前 BGM 轨控制增益的乘积。
/// 非正结果直接跳过读取，既产生静音，也避免为静音轨触碰流式缓存。
/// @warning 音频热路径；循环次数只与活跃片段、声道和请求帧数相关。
void AudioTimelineMixerNode::mixSegment(ice::AudioBuffer& output,
                                        std::size_t       outputStartFrame,
                                        std::size_t       frameCount)
{
    if ( !m_scheduleState ) return;
    auto& schedule = *m_scheduleState;
    // consume 同时推进位置；即使无活跃片段，也必须消费完整静音时间区间。
    const auto result = schedule.transport.consumeActiveSpans(
        frameCountFromSize(frameCount), std::span(schedule.activeSpanScratch));
    // 容量按片段总数预分配，truncated 仅作为契约破坏时的实时安全兜底。
    if ( result.truncated ) return;

    const auto outputChannels = output.num_channels();
    for ( std::size_t spanIndex = 0U; spanIndex < result.writtenSpanCount;
          ++spanIndex ) {
        const auto& span = schedule.activeSpanScratch[spanIndex];
        const auto& clip = schedule.clips[span.clipIndex];
        // 运行时控制是稳定观察对象，读取不会复制所有权或分配容器。
        const float runtimeGain =
            m_keySoundControls
                ? m_keySoundControls->effectiveBgmTrackGain(clip.bgmTrackIndex)
                : 1.0F;
        const float effectiveVolume = span.volume * runtimeGain;
        if ( effectiveVolume <= 0.0F ) continue;

        // 每个 span 复用同一 scratch；先清零以覆盖流式短读和缺失声道尾部。
        schedule.sourceScratch.clear();
        const auto requestedFrames = static_cast<std::size_t>(span.frameCount);
        const auto readFrames      = std::min<std::size_t>(
            clip.audio->read(schedule.sourceScratch,
                             static_cast<std::size_t>(span.sourceStartFrame),
                             requestedFrames),
            requestedFrames);
        // span.outputStartFrame 相对本段，外层偏移把它映射到整个设备 block。
        const auto destinationStart =
            outputStartFrame + static_cast<std::size_t>(span.outputStartFrame);
        const auto channels = std::min<std::size_t>(
            outputChannels, schedule.sourceScratch.num_channels());

        // 按确定片段顺序累加，重叠采样自然叠加而不覆盖已有输出。
        for ( std::size_t channel = 0U; channel < channels; ++channel ) {
            float* destination  = output.raw_ptrs()[channel] + destinationStart;
            const float* source = schedule.sourceScratch.raw_ptrs()[channel];
            for ( std::size_t frame = 0U; frame < readFrames; ++frame ) {
                destination[frame] += source[frame] * effectiveVolume;
            }
        }
    }
}

/// @brief 对完成混合的 block 应用主增益并发布左右峰值。
/// @param output 将被原地缩放的设备输出。
///
/// 峰值取缩放后的绝对样本最大值，因此与用户实际听到的输出响度一致。
/// 只发布前两个声道；单声道把同一峰值复制给左右表头，多余声道仍正常缩放。
/// 空缓冲显式归零，防止暂停、设备重配或零帧回调继续展示旧电平。
/// @warning 音频热路径；只进行一次样本遍历和两个 relaxed 原子写入。
void AudioTimelineMixerNode::applyMasterGainAndPublishLevels(
    ice::AudioBuffer& output) noexcept
{
    if ( output.num_frames() == 0U || output.raw_ptrs() == nullptr ) {
        m_leftLevel.store(0.0F, std::memory_order_relaxed);
        m_rightLevel.store(0.0F, std::memory_order_relaxed);
        return;
    }

    // block 起点只读取一次增益，使同一输出块不会出现中途参数撕裂。
    const float       gain      = m_masterGain.load(std::memory_order_relaxed);
    const std::size_t channels  = output.num_channels();
    float             leftPeak  = 0.0F;
    float             rightPeak = 0.0F;

    // 所有声道都应用主增益，但 UI 电平只保存标准左右两路。
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
    // 单声道在表头上应居中显示，避免右侧被误解为无输出。
    if ( channels == 1U ) rightPeak = leftPeak;
    m_leftLevel.store(leftPeak, std::memory_order_relaxed);
    m_rightLevel.store(rightPeak, std::memory_order_relaxed);
}

/// @brief 通过单写序列锁发布播放状态命令与提交时间。
/// @param command Stop、Play 或 Pause 控制意图。
///
/// 第一次递增把版本变为奇数，payload 写完后第二次递增提交偶数版本。
/// 音频线程读取失败时保留旧命令并在下一 block 重试，不会阻塞调用者或回调。
void AudioTimelineMixerNode::requestPlaybackCommand(
    PlaybackCommand command) noexcept
{
    // command 与时间戳必须作为一个事务发布，供 clockSnapshot 合并即时状态。
    m_playbackCommandSequence.fetch_add(1U, std::memory_order_acq_rel);
    m_requestedPlaybackCommand.store(command, std::memory_order_relaxed);
    m_requestedPlaybackSteadyTimeNanoseconds.store(steadyNowNanoseconds(),
                                                   std::memory_order_relaxed);
    m_playbackCommandSequence.fetch_add(1U, std::memory_order_release);
}

}  // namespace MMM::Audio
