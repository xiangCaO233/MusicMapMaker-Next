#include "log/colorful-log.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ice/core/IAudioNode.hpp>
#include <ice/core/MixBus.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <memory>
#include <new>
#include <string_view>
#include <thread>

namespace
{
// 本测试验证 MixBus 的固定容量混音与读复制更新协议：
//
// - 来源列表由控制线程发布为不可变快照，回调按稳定插入顺序读取；
// - prepare 后所有 scratch 地址和容量固定，短块与超长分块都不分配；
// - 重复来源被忽略，删除后重新加入会移动到稳定顺序尾部；
// - replace_source 保留索引，只发布一份完整的新来源快照；
// - 格式或来源活动帧数违约时整块静音并累计诊断；
// - 音频线程持有 hazard 快照期间，控制线程不能释放其中的来源；
// - 退役快照只在回调离开临界区后由控制线程回收。
//
// 分配计数采用 thread_local，控制线程创建新快照的合法分配不会被误计入音频
// 线程。测试代理覆盖普通、数组、带尺寸和对齐 new/delete，避免编译器选择
// 不同重载后漏报。所有计数在进入被测区间前清零，退出后立即关闭。
// CallTrace 也使用固定数组和原子计数，不会让测试观察逻辑自身制造回调分配。
// 样本节点只写常量，使来源顺序、累加次数和失败静音都能直接从输出识别。
// 测试的 FORMAT 与容量在栈上固定，不依赖全局音频设备配置。

/// @brief 当前线程是否正在统计普通堆分配。
thread_local bool g_trackAllocations{ false };

/// @brief 当前统计区间内发生的普通堆分配次数。
thread_local std::size_t g_allocationCount{ 0U };

/// @brief 当前统计区间内发生的普通堆释放次数。
thread_local std::size_t g_deallocationCount{ 0U };

/// @brief 记录断言失败并返回可累计的失败数量。
/// @param condition 需要成立的条件。
/// @param label 失败时输出的测试标签。
/// @return 条件成立时返回 0，否则返回 1。
int expectTrue(bool condition, std::string_view label)
{
    if ( condition ) return 0;
    XERROR("MixBus realtime safety assertion failed: {}", label);
    return 1;
}

/// @brief 来源调用顺序的无分配固定容量记录。
struct CallTrace {
    /// @brief 重置已写入的调用数量。
    void reset() { count.store(0U, std::memory_order_relaxed); }

    /// @brief 记录一次来源调用。
    /// @param id 来源编号。
    void append(int id, const ice::AudioBuffer& buffer)
    {
        // fetch_add 为每次调用保留唯一槽；超出测试容量时只停止记录，不分配。
        const std::size_t index =
            count.fetch_add(1U, std::memory_order_relaxed);
        if ( index >= ids.size() ) return;
        ids[index]                  = id;
        activeFrames[index]         = buffer.num_frames();
        frameCapacities[index]      = buffer.frame_capacity();
        const float* const* samples = buffer.raw_ptrs();
        storageAddresses[index]     = samples ? samples[0] : nullptr;
    }

    /// @brief 固定容量的来源编号序列。
    std::array<int, 32U> ids{};

    /// @brief 每次来源调用观察到的逻辑帧数。
    std::array<std::size_t, 32U> activeFrames{};

    /// @brief 每次来源调用观察到的固定存储容量。
    std::array<std::size_t, 32U> frameCapacities{};

    /// @brief 每次来源调用观察到的首声道存储地址。
    std::array<const float*, 32U> storageAddresses{};

    /// @brief 已发生的来源调用数量。
    std::atomic<std::size_t> count{ 0U };
};

/// @brief 写入固定值并记录调用顺序的无分配测试节点。
class ConstantNode final : public ice::IAudioNode
{
public:
    /// @brief 构造固定值节点。
    /// @param id 来源编号。
    /// @param value 写入每个采样的固定值。
    /// @param trace 调用顺序记录。
    ConstantNode(int id, float value, CallTrace& trace)
        : m_id(id), m_value(value), m_trace(trace)
    {
    }

    /// @brief 将固定值写入整个缓冲区。
    /// @param buffer 输出缓冲区。
    /// @warning 测试音频热路径：不得分配内存。
    void process(ice::AudioBuffer& buffer) override
    {
        // 先记录 MixBus 暴露的缓冲视图，再用固定值覆盖整个活动前缀。
        m_trace.append(m_id, buffer);
        float** samples = buffer.raw_ptrs();
        if ( !samples ) return;
        for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
              ++channel ) {
            for ( std::size_t frame = 0U; frame < buffer.num_frames();
                  ++frame ) {
                samples[channel][frame] = m_value;
            }
        }
    }

private:
    /// @brief 来源编号。
    int m_id;

    /// @brief 每个采样写入的固定值。
    float m_value;

    /// @brief 非拥有的调用顺序记录。
    CallTrace& m_trace;
};

/// @brief 故意破坏逻辑帧数以验证 MixBus 失败静音的测试节点。
class InvalidFrameNode final : public ice::IAudioNode
{
public:
    /// @brief 将逻辑帧数缩短一帧。
    /// @param buffer 待破坏的输入缓冲区。
    /// @warning 测试音频热路径：只修改活动帧数，不分配内存。
    void process(ice::AudioBuffer& buffer) override
    {
        // 故意违反“来源保持活动帧数不变”的契约，触发总线整块拒绝。
        const std::size_t invalidFrames =
            buffer.num_frames() > 0U ? buffer.num_frames() - 1U : 0U;
        static_cast<void>(buffer.set_active_frames(invalidFrames));
    }
};

/// @brief 阻塞节点与测试线程共享的生命周期状态。
struct BlockingState {
    // 状态独立于 BlockingNode 生命周期，析构后测试仍能读取最终标志。
    /// @brief process 是否已经进入。
    std::atomic_bool entered{ false };

    /// @brief 是否允许 process 返回。
    std::atomic_bool release{ false };

    /// @brief 节点是否已经析构。
    std::atomic_bool destroyed{ false };

    /// @brief 节点是否在 process 尚未返回时被析构。
    std::atomic_bool destroyedWhileProcessing{ false };
};

/// @brief 用于验证快照回收不会提前销毁来源的阻塞节点。
class BlockingNode final : public ice::IAudioNode
{
public:
    /// @brief 构造阻塞节点。
    /// @param state 外部持有的生命周期状态。
    explicit BlockingNode(std::shared_ptr<BlockingState> state)
        : m_state(std::move(state))
    {
    }

    /// @brief 记录析构时机。
    ~BlockingNode() override
    {
        // release 尚未发布便析构，说明 hazard 快照没有覆盖完整 process
        // 生命周期。
        if ( m_state->entered.load(std::memory_order_acquire) &&
             !m_state->release.load(std::memory_order_acquire) ) {
            m_state->destroyedWhileProcessing.store(true,
                                                    std::memory_order_release);
        }
        m_state->destroyed.store(true, std::memory_order_release);
    }

    /// @brief 阻塞至控制线程允许返回。
    /// @param buffer 输出缓冲区。
    /// @warning 仅测试 RCU 临界区；这里的 yield 不代表生产回调实现。
    void process(ice::AudioBuffer& buffer) override
    {
        // entered 与 release 建立确定并发窗口；循环只存在于测试节点。
        m_state->entered.store(true, std::memory_order_release);
        while ( !m_state->release.load(std::memory_order_acquire) ) {
            std::this_thread::yield();
        }

        float** samples = buffer.raw_ptrs();
        if ( !samples ) return;
        for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
              ++channel ) {
            for ( std::size_t frame = 0U; frame < buffer.num_frames();
                  ++frame ) {
                samples[channel][frame] = 0.25F;
            }
        }
    }

private:
    /// @brief 保证节点析构后仍可检查的共享状态。
    std::shared_ptr<BlockingState> m_state;
};

/// @brief 检查缓冲区的全部采样是否接近期望值。
/// @param buffer 待检查缓冲区。
/// @param expected 期望采样值。
/// @return 全部采样符合预期时返回 true。
bool allSamplesEqual(const ice::AudioBuffer& buffer, float expected)
{
    const float* const* samples = buffer.raw_ptrs();
    if ( !samples ) return buffer.num_frames() == 0U;
    for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < buffer.num_frames(); ++frame ) {
            if ( std::abs(samples[channel][frame] - expected) > 0.00001F ) {
                return false;
            }
        }
    }
    return true;
}

/// @brief 验证稳定来源顺序、回调普通堆零分配和超长 block 分块。
/// @return 失败断言数量。
///
/// 两个 ConstantNode 的输出分别为 1 和 2，正确累加后每个样本应为 3。来源
/// 调用轨迹同时记录活动帧数、固定容量和首声道地址，用于证明 10 帧超长 block
/// 被拆成 4、4、2 三段并始终复用同一 scratch。
///
/// 场景还逐项覆盖：
///
/// - 重复 add 后来源数量保持二；
/// - 三帧短 block 仍复用四帧 scratch；
/// - remove/add 后顺序从 1、2 变为 2、1；
/// - 超长 block 的每个分块都按 2、1 调用；
/// - 最后两帧只改变 active frame，不缩小容量或换地址；
/// - 错误声道数时输出归零并递增 rejected 计数；
/// - 来源破坏 active frame 时整块归零并再次递增计数；
/// - clear 发布空来源快照，查询数量立即为零。
///
/// 短块路径验证 prepare 容量是上限而不是固定活动长度；超长路径验证上限又能
/// 被内部安全分块复用。两者结合可防止实现把每次设备 block 大小误当作分配依据。
/// 存储地址六次完全相同，进一步证明最后两帧分块只调整逻辑视图而未换缓冲。
int testStableOrderAndRealtimeCapacity()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t PREPARED_FRAMES = 4U;

    CallTrace trace;
    auto      first  = std::make_shared<ConstantNode>(1, 1.0F, trace);
    auto      second = std::make_shared<ConstantNode>(2, 2.0F, trace);

    ice::MixBus bus;
    bus.prepare(FORMAT, PREPARED_FRAMES);
    // 重复加入 first 不得形成双重混音，sourceCount 应保持二。
    bus.add_source(first);
    bus.add_source(second);
    bus.add_source(first);

    int failures = 0;
    failures +=
        expectTrue(bus.sourceCount() == 2U, "duplicate source is ignored");
    failures += expectTrue(bus.maxPreparedFrames() == PREPARED_FRAMES,
                           "prepare publishes requested frame capacity");

    ice::AudioBuffer shortBuffer(FORMAT, 3U);
    // 计数只覆盖 process，测试对象和输出缓冲均已在控制阶段构造完成。
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    bus.process(shortBuffer);
    g_trackAllocations = false;

    failures += expectTrue(g_allocationCount == 0U && g_deallocationCount == 0U,
                           "prepared callback performs no ordinary heap "
                           "allocation or deallocation");
    failures += expectTrue(allSamplesEqual(shortBuffer, 3.0F),
                           "sources are mixed into prepared callback output");
    failures += expectTrue(trace.count.load(std::memory_order_relaxed) == 2U &&
                               trace.ids[0] == 1 && trace.ids[1] == 2,
                           "sources keep insertion order");

    // 删除再添加是显式顺序变更，下一快照应按 second、first 调用。
    bus.remove_source(first);
    bus.add_source(first);
    trace.reset();
    bus.process(shortBuffer);
    failures += expectTrue(trace.count.load(std::memory_order_relaxed) == 2U &&
                               trace.ids[0] == 2 && trace.ids[1] == 1,
                           "remove then add moves source to stable tail");

    trace.reset();
    // 10 不能被预备容量 4 整除，最后一段必须只暴露两个活动帧。
    ice::AudioBuffer oversizedBuffer(FORMAT, 10U);
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    bus.process(oversizedBuffer);
    g_trackAllocations = false;
    failures += expectTrue(g_allocationCount == 0U && g_deallocationCount == 0U,
                           "oversized callback chunks without ordinary heap "
                           "allocation or deallocation");
    failures += expectTrue(allSamplesEqual(oversizedBuffer, 3.0F),
                           "oversized callback mixes every chunk");
    failures += expectTrue(bus.oversizedProcessCount() == 1U,
                           "oversized callback increments diagnostic");
    failures += expectTrue(trace.count.load(std::memory_order_relaxed) == 6U &&
                               trace.ids[0] == 2 && trace.ids[1] == 1 &&
                               trace.ids[2] == 2 && trace.ids[3] == 1 &&
                               trace.ids[4] == 2 && trace.ids[5] == 1,
                           "chunk processing preserves source order");
    failures += expectTrue(
        trace.activeFrames[0] == 4U && trace.activeFrames[1] == 4U &&
            trace.activeFrames[2] == 4U && trace.activeFrames[3] == 4U &&
            trace.activeFrames[4] == 2U && trace.activeFrames[5] == 2U,
        "non-divisible oversized block exposes exact active frame counts");
    failures += expectTrue(
        trace.frameCapacities[0] == PREPARED_FRAMES &&
            trace.frameCapacities[1] == PREPARED_FRAMES &&
            trace.frameCapacities[2] == PREPARED_FRAMES &&
            trace.frameCapacities[3] == PREPARED_FRAMES &&
            trace.frameCapacities[4] == PREPARED_FRAMES &&
            trace.frameCapacities[5] == PREPARED_FRAMES &&
            trace.storageAddresses[0] != nullptr &&
            trace.storageAddresses[0] == trace.storageAddresses[1] &&
            trace.storageAddresses[0] == trace.storageAddresses[2] &&
            trace.storageAddresses[0] == trace.storageAddresses[3] &&
            trace.storageAddresses[0] == trace.storageAddresses[4] &&
            trace.storageAddresses[0] == trace.storageAddresses[5],
        "chunk active-frame changes preserve scratch capacity and address");

    // 格式拒绝不得调用来源，且输出必须覆盖为确定静音。
    constexpr ice::AudioDataFormat WRONG_FORMAT{
        .channels   = 1U,
        .samplerate = 48000U,
    };
    ice::AudioBuffer wrongFormatBuffer(WRONG_FORMAT, 3U);
    bus.process(wrongFormatBuffer);
    failures += expectTrue(allSamplesEqual(wrongFormatBuffer, 0.0F),
                           "format mismatch returns deterministic silence");
    failures += expectTrue(bus.rejectedProcessCount() == 1U,
                           "format mismatch increments rejection diagnostic");

    // 来源若擅自缩短活动帧数，MixBus 不能混合一份形状不一致的 block。
    bus.clear();
    auto invalidSource = std::make_shared<InvalidFrameNode>();
    bus.add_source(invalidSource);
    ice::AudioBuffer invalidOutput(FORMAT, 3U);
    bus.process(invalidOutput);
    failures += expectTrue(allSamplesEqual(invalidOutput, 0.0F),
                           "invalid source frame count silences whole block");
    failures += expectTrue(bus.rejectedProcessCount() == 2U,
                           "invalid source frame count increments diagnostic");

    bus.clear();
    failures += expectTrue(bus.sourceCount() == 0U,
                           "clear publishes an empty source snapshot");
    return failures;
}

/// @brief 验证 replace_source 只发布一次快照且保留原来源索引。
/// @return 失败断言数量。
///
/// 初始顺序为 1、2、3，把中间来源替换为值 9 后，调用顺序必须为 1、9、3，
/// 输出和为 13。不存在的旧来源与已存在的新来源都应被拒绝，且不改变当前快照。
/// sourceCount 在替换前后均为三，确保实现没有短暂删除或重复插入。输出和与
/// CallTrace 同时断言，避免某个来源被调用两次但数值偶然抵消后漏检。
/// 两个拒绝分支之后不再 process；它们的返回值直接证明当前有效快照未被发布。
int testIndexPreservingReplacement()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };

    CallTrace trace;
    auto      first       = std::make_shared<ConstantNode>(1, 1.0F, trace);
    auto      second      = std::make_shared<ConstantNode>(2, 2.0F, trace);
    auto      third       = std::make_shared<ConstantNode>(3, 3.0F, trace);
    auto      replacement = std::make_shared<ConstantNode>(9, 9.0F, trace);

    ice::MixBus bus;
    bus.prepare(FORMAT, 8U);
    bus.add_source(first);
    bus.add_source(second);
    bus.add_source(third);

    int failures = 0;
    // 成功替换只改变一个索引，不执行 remove 加 add 造成的尾部移动。
    failures += expectTrue(bus.replace_source(second, replacement),
                           "replace_source accepts an existing source");
    failures += expectTrue(bus.sourceCount() == 3U,
                           "replace_source preserves source count");

    ice::AudioBuffer buffer(FORMAT, 8U);
    bus.process(buffer);
    failures += expectTrue(
        trace.count.load(std::memory_order_relaxed) == 3U &&
            trace.ids[0] == 1 && trace.ids[1] == 9 && trace.ids[2] == 3,
        "replace_source preserves the replaced source index");
    failures += expectTrue(allSamplesEqual(buffer, 13.0F),
                           "replacement source participates exactly once");
    // second 已不在新快照中，因此不能再次作为匹配键。
    failures += expectTrue(!bus.replace_source(second, first),
                           "replace_source rejects an absent source");
    failures += expectTrue(!bus.replace_source(replacement, first),
                           "replace_source rejects a duplicate replacement");
    return failures;
}

/// @brief 验证控制线程替换来源时 hazard 快照保护其完整 process 生命周期。
/// @return 失败断言数量。
///
/// BlockingNode 在 process 内等待测试门，确保控制线程在回调临界区期间替换来源
/// 并尝试回收。旧来源的 weak_ptr 此时必须仍有效，退役队列也必须保留被 hazard
/// 保护的快照。放行并 join 后再次回收，析构才可发生；下一 block
/// 只调用替代节点。
/// @warning 测试中的阻塞只用于确定性放大并发窗口，不代表生产音频实现。
///
/// 第一次 reclaim 发生在旧节点仍阻塞时，预期退役计数至少为一。第二次发生在
/// processor join 后，预期 weak_ptr 过期且退役计数归零。额外析构标志单独
/// 防止实现虽然延迟快照，却提前释放快照内部的来源对象。
/// 替代节点固定输出 0.5，并用独立 CallTrace 确认接管后的首块只调用一次。
/// 旧 BlockingNode 输出 0.25 不参与最终断言，避免并发首块内容影响生命周期验证。
int testConcurrentSnapshotReplacement()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };

    auto                        state  = std::make_shared<BlockingState>();
    auto                        source = std::make_shared<BlockingNode>(state);
    std::weak_ptr<BlockingNode> oldSource = source;

    ice::MixBus bus;
    bus.prepare(FORMAT, 16U);
    bus.add_source(source);

    ice::AudioBuffer buffer(FORMAT, 16U);
    // 等待 entered 确保音频线程已经取得旧快照，而非仅仅创建了线程。
    std::thread processor([&bus, &buffer]() { bus.process(buffer); });
    while ( !state->entered.load(std::memory_order_acquire) ) {
        std::this_thread::yield();
    }

    CallTrace replacementTrace;
    auto      replacement =
        std::make_shared<ConstantNode>(7, 0.5F, replacementTrace);
    // 控制线程释放自己的强引用后，旧来源只应由活跃 hazard 快照保留。
    const bool replaced = bus.replace_source(source, replacement);
    source.reset();
    bus.reclaimRetiredSources();

    int failures = 0;
    failures +=
        expectTrue(replaced, "concurrent replace_source publishes replacement");
    failures += expectTrue(!oldSource.expired(),
                           "hazard snapshot retains source during process");
    failures += expectTrue(bus.retiredSnapshotCount() >= 1U,
                           "protected snapshot remains in retired queue");
    failures += expectTrue(
        !state->destroyedWhileProcessing.load(std::memory_order_acquire),
        "source is not destroyed inside process");

    // 回调退出临界区后，控制线程才可回收快照并触发旧来源析构。
    state->release.store(true, std::memory_order_release);
    processor.join();
    bus.reclaimRetiredSources();

    failures += expectTrue(oldSource.expired(),
                           "source is reclaimed after callback quiesces");
    failures += expectTrue(bus.retiredSnapshotCount() == 0U,
                           "retired snapshots are fully reclaimed");
    failures += expectTrue(
        state->destroyed.load(std::memory_order_acquire) &&
            !state->destroyedWhileProcessing.load(std::memory_order_acquire),
        "source destructor runs only after process returns");

    // 新 block 必须只观察替代来源一次，证明快照切换没有重复或遗漏。
    bus.process(buffer);
    failures += expectTrue(
        allSamplesEqual(buffer, 0.5F) &&
            replacementTrace.count.load(std::memory_order_relaxed) == 1U,
        "next block observes replacement snapshot once");
    return failures;
}
}  // namespace

/// @brief 为回调零分配测试代理普通单对象堆分配。
/// @param size 请求字节数。
/// @return 已分配内存。
void* operator new(std::size_t size)
{
    if ( g_trackAllocations ) ++g_allocationCount;
    if ( void* memory = std::malloc(size) ) return memory;
    std::abort();
}

/// @brief 为回调零分配测试代理普通数组堆分配。
/// @param size 请求字节数。
/// @return 已分配内存。
void* operator new[](std::size_t size)
{
    if ( g_trackAllocations ) ++g_allocationCount;
    if ( void* memory = std::malloc(size) ) return memory;
    std::abort();
}

/// @brief 释放普通单对象堆内存。
/// @param memory 待释放内存。
void operator delete(void* memory) noexcept
{
    if ( g_trackAllocations ) ++g_deallocationCount;
    std::free(memory);
}

/// @brief 释放普通数组堆内存。
/// @param memory 待释放内存。
void operator delete[](void* memory) noexcept
{
    if ( g_trackAllocations ) ++g_deallocationCount;
    std::free(memory);
}

/// @brief 释放带尺寸的普通单对象堆内存。
/// @param memory 待释放内存。
/// @param size 原始请求字节数。
void operator delete(void* memory, std::size_t size) noexcept
{
    static_cast<void>(size);
    if ( g_trackAllocations ) ++g_deallocationCount;
    std::free(memory);
}

/// @brief 释放带尺寸的普通数组堆内存。
/// @param memory 待释放内存。
/// @param size 原始请求字节数。
void operator delete[](void* memory, std::size_t size) noexcept
{
    static_cast<void>(size);
    if ( g_trackAllocations ) ++g_deallocationCount;
    std::free(memory);
}

/// @brief 为回调零分配测试代理对齐单对象堆分配。
/// @param size 请求字节数。
/// @param alignment 请求对齐。
/// @return 已分配内存。
void* operator new(std::size_t size, std::align_val_t alignment)
{
    if ( g_trackAllocations ) ++g_allocationCount;
    const std::size_t alignmentBytes = static_cast<std::size_t>(alignment);
    const std::size_t alignedSize =
        ((size + alignmentBytes - 1U) / alignmentBytes) * alignmentBytes;
    if ( void* memory = std::aligned_alloc(alignmentBytes, alignedSize) ) {
        return memory;
    }
    std::abort();
}

/// @brief 为回调零分配测试代理对齐数组堆分配。
/// @param size 请求字节数。
/// @param alignment 请求对齐。
/// @return 已分配内存。
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return ::operator new(size, alignment);
}

/// @brief 释放对齐单对象堆内存。
/// @param memory 待释放内存。
/// @param alignment 原始请求对齐。
void operator delete(void* memory, std::align_val_t alignment) noexcept
{
    static_cast<void>(alignment);
    if ( g_trackAllocations ) ++g_deallocationCount;
    std::free(memory);
}

/// @brief 释放对齐数组堆内存。
/// @param memory 待释放内存。
/// @param alignment 原始请求对齐。
void operator delete[](void* memory, std::align_val_t alignment) noexcept
{
    ::operator delete(memory, alignment);
}

/// @brief 释放带尺寸的对齐单对象堆内存。
/// @param memory 待释放内存。
/// @param size 原始请求字节数。
/// @param alignment 原始请求对齐。
void operator delete(void* memory, std::size_t size,
                     std::align_val_t alignment) noexcept
{
    static_cast<void>(size);
    ::operator delete(memory, alignment);
}

/// @brief 释放带尺寸的对齐数组堆内存。
/// @param memory 待释放内存。
/// @param size 原始请求字节数。
/// @param alignment 原始请求对齐。
void operator delete[](void* memory, std::size_t size,
                       std::align_val_t alignment) noexcept
{
    static_cast<void>(size);
    ::operator delete(memory, alignment);
}

/// @brief 运行 MixBus 实时安全和并发快照测试。
/// @return 测试通过时返回 0。
///
/// 三个场景分别覆盖容量与顺序、单线程替换语义、并发生命周期。失败数统一
/// 累加，便于一次执行报告所有独立契约，而不把某一类失败误判为其他问题。
/// 测试不启动真实音频设备，所有 block 由当前线程或显式 processor 线程驱动，
/// 因而每个控制发布点和回收点都可重复复现。
/// 全局分配代理只存在于本测试二进制，测试结束后不会影响其他音频目标。
/// operator new 失败时直接 abort，与标准不可恢复分配失败语义保持一致。
/// 断言失败数非零时返回一，适合作为 CTest 的单一成功条件。
int main()
{
    const int failures = testStableOrderAndRealtimeCapacity() +
                         testIndexPreservingReplacement() +
                         testConcurrentSnapshotReplacement();
    if ( failures != 0 ) {
        XERROR("MixBus realtime safety tests failed: {}", failures);
        return 1;
    }
    return 0;
}
