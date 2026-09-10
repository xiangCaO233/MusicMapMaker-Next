#include "log/colorful-log.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <ice/config/config.hpp>
#include <ice/core/SourceNode.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <ice/manage/AudioPool.hpp>
#include <ice/thread/ThreadPool.hpp>
#include <memory>
#include <string_view>
#include <thread>

namespace
{
// 本测试覆盖 SourceNode 的定时起播与 provider 热替换协议：
//
// - 绝对起播帧由外部参考时钟映射到当前 block 内偏移；
// - 相对延迟在调用方输出帧域递减，不提前推进解码源位置；
// - 等待区间严格静音，跨过起点后直接写入同一预分配输出缓冲；
// - 格式不匹配返回静音并增加诊断，不在回调临时转换；
// - 最后一批有效输入在所属 block 内同步通知且每轮只通知一次；
// - 控制线程替换 provider 时，正在调用的闭包由 hazard 状态保持存活；
// - provider 退役资源只在音频回调离开后由控制线程回收。
//
// 测试侧分配探针只统计当前音频线程，资源加载、provider 发布与参考缓冲构造
// 均在计数窗口之外完成。样本真值由未调度的独立 SourceNode 生成，不复用被测
// 定时偏移算法。并发测试中的 yield 只用于制造可重复的临界区。
// 比较阈值固定为一百万分之一，只容纳 PCM 浮点复制误差，不掩盖帧错位。
// 所有区间使用半开坐标，START_OFFSET 同时表示静音前缀长度与 PCM 写入起点。
// 失败数可累计，单一场景中的多个独立实时契约会在同次执行中得到诊断。

/// @brief 当前线程是否正在统计普通堆分配。
thread_local bool g_trackAllocations{ false };

/// @brief 当前统计区间内发生的普通堆分配次数。
thread_local std::size_t g_allocationCount{ 0U };

/// @brief 记录断言失败并返回可累计的失败数量。
/// @param condition 需要成立的条件。
/// @param label 失败时输出的测试标签。
/// @return 条件成立时返回 0，否则返回 1。
int expectTrue(bool condition, std::string_view label)
{
    if ( condition ) return 0;
    XERROR("SourceNode scheduled realtime assertion failed: {}", label);
    return 1;
}

/// @brief 可由轻量 provider 读取的常驻原子时钟。
struct ReferenceClock {
    /// @brief 对象地址在 provider 注册至回收期间保持稳定。
    /// @brief 当前 block 起点帧。
    std::atomic<std::size_t> frame{ 0U };
};

/// @brief 从常驻原子时钟读取参考帧。
/// @param context 指向 ReferenceClock 的非空地址。
/// @return 当前参考帧。
std::size_t readReferenceClock(const void* context) noexcept
{
    // 原始 provider 不取得 context 所有权，只执行单次 relaxed 读取。
    const auto* clock = static_cast<const ReferenceClock*>(context);
    return clock ? clock->frame.load(std::memory_order_relaxed) : 0U;
}

/// @brief 兼容 provider 并发测试使用的阻塞门。
struct ProviderGate {
    /// @brief gate 由测试线程共享所有权，闭包析构不影响观察状态。
    /// @brief provider 是否已经进入。
    std::atomic_bool entered{ false };

    /// @brief 是否允许 provider 返回。
    std::atomic_bool release{ false };
};

/// @brief 记录 SourceNode 同块输入结束通知。
struct FinalInputProbe {
    /// @brief probe 生命周期覆盖 SourceNode 的 listener 注册期。
    /// @brief 被观察的音源节点。
    ice::SourceNode* source{ nullptr };

    /// @brief 已收到的通知次数。
    std::atomic<std::size_t> notifications{ 0U };

    /// @brief 通知发生时观察到的源帧位置。
    std::atomic<std::size_t> observedPosition{ 0U };
};

/// @brief 记录 SourceNode 的轻量输入结束通知。
/// @param context 指向 FinalInputProbe 的稳定地址。
void recordFinalInput(void* context) noexcept
{
    // 先读取回调发生时的位置，再递增计数，测试线程在 process 返回后观察。
    auto* probe = static_cast<FinalInputProbe*>(context);
    if ( !probe ) return;
    if ( probe->source ) {
        probe->observedPosition.store(probe->source->get_playpos(),
                                      std::memory_order_relaxed);
    }
    probe->notifications.fetch_add(1U, std::memory_order_relaxed);
}

/// @brief 验证 block 内指定区间是否全部为静音。
/// @param buffer 待检查缓冲区。
/// @param begin 起始帧。
/// @param end 排除结束帧。
/// @return 区间内全部采样接近零时返回 true。
bool isSilent(const ice::AudioBuffer& buffer, std::size_t begin,
              std::size_t end)
{
    // 区间采用半开语义，与 block 内起播偏移和输出切片保持一致。
    const float* const* samples = buffer.raw_ptrs();
    if ( !samples || begin > end || end > buffer.num_frames() ) return false;
    for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
          ++channel ) {
        for ( std::size_t frame = begin; frame < end; ++frame ) {
            if ( std::abs(samples[channel][frame]) > 0.000001F ) return false;
        }
    }
    return true;
}

/// @brief 比较定时起播片段与从零帧直接读取的参考片段。
/// @param scheduled 包含静音前缀的定时输出。
/// @param scheduledOffset 定时输出内的起播偏移。
/// @param reference 从源零帧直接读取的参考输出。
/// @return 对应采样一致时返回 true。
bool matchesReference(const ice::AudioBuffer& scheduled,
                      std::size_t             scheduledOffset,
                      const ice::AudioBuffer& reference)
{
    // 在进入逐样本比较前验证形状与偏移，避免测试 helper 自身越界。
    const float* const* scheduledSamples = scheduled.raw_ptrs();
    const float* const* referenceSamples = reference.raw_ptrs();
    if ( !scheduledSamples || !referenceSamples ||
         scheduled.num_channels() != reference.num_channels() ||
         scheduledOffset + reference.num_frames() > scheduled.num_frames() ) {
        return false;
    }

    for ( std::uint16_t channel = 0U; channel < reference.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < reference.num_frames();
              ++frame ) {
            if ( std::abs(scheduledSamples[channel][scheduledOffset + frame] -
                          referenceSamples[channel][frame]) > 0.000001F ) {
                return false;
            }
        }
    }
    return true;
}

/// @brief 验证 block 内起播无需临时缓冲或普通堆分配。
/// @param track 已完整缓存的测试音轨。
/// @return 失败断言数量。
///
/// 首个绝对定时场景把起点放在 64 帧 block 的第 32 帧：前缀必须静音，后缀
/// 必须与独立 SourceNode 从源零点读取的 32 帧一致，且源位置只前进 32。
/// 随后把起点放到两个 block 之外验证整块等待，再覆盖错误格式拒绝。
/// 最后用 96 帧相对延迟验证首块全静音、第二块在偏移 32 精确起播。
///
/// 该场景的重要状态断言包括：
///
/// - 绝对目标跨过后 scheduledStartFrame 清零；
/// - 源 playpos 只累计真正解码的后缀帧；
/// - 尚未跨过绝对目标时，目标值与 playpos 均保持；
/// - 错误格式只递增一次 rejectedProcessCount；
/// - 相对延迟先从 96 降到 32，再在第二块归零；
/// - 两种 block 内起播路径在计数区间都没有普通堆分配。
///
/// 错误格式测试复用仍有绝对目标的 scheduledSource，证明拒绝不会消费调度状态。
/// 相对延迟使用新节点，避免之前绝对 provider 或 rejected 计数干扰结果。
int testBlockLocalStartAndZeroAllocation(
    const std::shared_ptr<ice::AudioTrack>& track)
{
    constexpr std::size_t BLOCK_FRAMES = 64U;
    constexpr std::size_t START_OFFSET = 32U;

    // 参考节点不设置调度，只生成起播后应出现的源 PCM 真值。
    ice::SourceNode referenceSource(track);
    referenceSource.setvolume(1.0F);
    referenceSource.play();
    ice::AudioBuffer referenceBuffer(ice::ICEConfig::internal_format,
                                     BLOCK_FRAMES - START_OFFSET);
    referenceSource.process(referenceBuffer);

    // 参考时钟初始为零，因此绝对目标 32 直接落在首块偏移 32。
    ReferenceClock  clock;
    ice::SourceNode scheduledSource(track);
    scheduledSource.setvolume(1.0F);
    scheduledSource.set_playpos(0U);
    scheduledSource.set_scheduled_start_frame(START_OFFSET);
    scheduledSource.set_reference_pos_provider(&clock, readReferenceClock);
    scheduledSource.play();

    ice::AudioBuffer scheduledBuffer(ice::ICEConfig::internal_format,
                                     BLOCK_FRAMES);
    // 计数窗口只包围定时 process，缓冲与 provider 均已在控制阶段准备。
    g_allocationCount  = 0U;
    g_trackAllocations = true;
    scheduledSource.process(scheduledBuffer);
    g_trackAllocations = false;

    int failures = 0;
    failures += expectTrue(g_allocationCount == 0U,
                           "block-local start performs no ordinary heap "
                           "allocation");
    failures += expectTrue(isSilent(scheduledBuffer, 0U, START_OFFSET),
                           "scheduled block keeps exact silent prefix");
    failures += expectTrue(
        matchesReference(scheduledBuffer, START_OFFSET, referenceBuffer),
        "scheduled block shifts decoded PCM to exact start frame");
    failures +=
        expectTrue(scheduledSource.get_playpos() == BLOCK_FRAMES - START_OFFSET,
                   "scheduled block advances only decoded source frames");
    failures += expectTrue(scheduledSource.scheduledStartFrame() == 0U,
                           "scheduled target clears after crossing");

    // 尚需等待两个完整 block 时，本块不读取资源也不清除目标。
    scheduledSource.set_playpos(0U);
    scheduledSource.set_scheduled_start_frame(BLOCK_FRAMES * 2U);
    ice::AudioBuffer waitingBuffer(ice::ICEConfig::internal_format,
                                   BLOCK_FRAMES);
    scheduledSource.process(waitingBuffer);
    failures += expectTrue(
        isSilent(waitingBuffer, 0U, BLOCK_FRAMES) &&
            scheduledSource.get_playpos() == 0U &&
            scheduledSource.scheduledStartFrame() == BLOCK_FRAMES * 2U,
        "whole waiting block remains silent and pending");

    // 格式错误不能进入解码或调度路径，完整输出应保持静音。
    constexpr ice::AudioDataFormat WRONG_FORMAT{
        .channels   = 1U,
        .samplerate = 48000U,
    };
    ice::AudioBuffer wrongFormatBuffer(WRONG_FORMAT, BLOCK_FRAMES);
    scheduledSource.process(wrongFormatBuffer);
    failures +=
        expectTrue(isSilent(wrongFormatBuffer, 0U, BLOCK_FRAMES) &&
                       scheduledSource.rejectedProcessCount() == 1U,
                   "format mismatch returns silence and increments diagnostic");

    // 相对延迟不依赖外部时钟，直接在每个 process 后扣除本块输出帧数。
    ice::SourceNode relativeSource(track);
    relativeSource.setvolume(1.0F);
    relativeSource.set_playpos(0U);
    relativeSource.set_scheduled_start_delay_frames(BLOCK_FRAMES +
                                                    START_OFFSET);
    relativeSource.play();
    ice::AudioBuffer firstRelativeBlock(ice::ICEConfig::internal_format,
                                        BLOCK_FRAMES);
    relativeSource.process(firstRelativeBlock);
    failures += expectTrue(
        isSilent(firstRelativeBlock, 0U, BLOCK_FRAMES) &&
            relativeSource.scheduledStartDelayFrames() == START_OFFSET &&
            relativeSource.get_playpos() == 0U,
        "relative delay decrements in caller output frame domain");

    // 第二块剩余延迟为 32，后 32 帧应从源零点开始且不发生分配。
    ice::AudioBuffer secondRelativeBlock(ice::ICEConfig::internal_format,
                                         BLOCK_FRAMES);
    g_allocationCount  = 0U;
    g_trackAllocations = true;
    relativeSource.process(secondRelativeBlock);
    g_trackAllocations = false;
    ice::AudioBuffer relativeReference(ice::ICEConfig::internal_format,
                                       BLOCK_FRAMES - START_OFFSET);
    ice::SourceNode  relativeReferenceSource(track);
    relativeReferenceSource.setvolume(1.0F);
    relativeReferenceSource.play();
    relativeReferenceSource.process(relativeReference);
    failures += expectTrue(
        g_allocationCount == 0U &&
            isSilent(secondRelativeBlock, 0U, START_OFFSET) &&
            matchesReference(
                secondRelativeBlock, START_OFFSET, relativeReference) &&
            relativeSource.scheduledStartDelayFrames() == 0U,
        "relative delay starts at exact in-block frame without allocation");
    return failures;
}

/// @brief 验证最后一批有效输入在同一次 process 内通知且每轮只通知一次。
/// @param track 已完整缓存的测试音轨。
/// @return 失败断言数量。
///
/// 从资源尾部前 16 帧开始拉取 64 帧，SourceNode 必须在本次 block 内读完内容、
/// 把播放位置推进到资源总帧数并同步调用 final listener。停止后的额外 process
/// 不得重复通知；重新定位到尾部前 8 帧并 Play 后属于新一轮，可再次通知一次。
/// listener 在 process 返回前观察到 playpos 已等于 totalFrames，证明通知发生在
/// 最后一批输入提交之后，而不是提前预告或依赖下一次空 block。
/// 测试不要求尾部补零的具体样本，只聚焦通知时机、位置与每轮幂等性。
int testSameBlockFinalInputNotification(
    const std::shared_ptr<ice::AudioTrack>& track)
{
    constexpr std::size_t BLOCK_FRAMES = 64U;
    const std::size_t     totalFrames  = track->num_frames();
    ice::SourceNode       source(track);
    FinalInputProbe       probe{ .source = &source };
    source.set_final_input_listener(&probe, recordFinalInput);
    source.setvolume(1.0F);
    // 剩余有效输入小于 block，正好覆盖同块结束与静音尾部。
    source.set_playpos(totalFrames - 16U);
    source.play();

    ice::AudioBuffer buffer(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    // 第一轮结束后再次拉取，确认 finished 通知具有每轮幂等性。
    source.process(buffer);

    int failures = 0;
    failures += expectTrue(
        probe.notifications.load(std::memory_order_relaxed) == 1U &&
            probe.observedPosition.load(std::memory_order_relaxed) ==
                totalFrames &&
            !source.isplaying(),
        "final listener runs in the block containing the last valid input");

    source.process(buffer);
    failures +=
        expectTrue(probe.notifications.load(std::memory_order_relaxed) == 1U,
                   "stopped source does not repeat final listener");

    // 显式重定位并播放开启新一轮通知资格。
    source.set_playpos(totalFrames - 8U);
    source.play();
    source.process(buffer);
    failures +=
        expectTrue(probe.notifications.load(std::memory_order_relaxed) == 2U,
                   "new playback cycle may notify final again");
    return failures;
}

/// @brief 验证控制线程替换兼容 provider 时不会与音频读取产生数据竞争。
/// @param track 已完整缓存的测试音轨。
/// @return 失败断言数量。
///
/// 首个闭包 provider 捕获 gate 与 token，并在音频线程调用中阻塞。控制线程此时
/// 替换为原始函数指针 provider 并尝试回收；token 的 weak_ptr 必须仍有效，证明
/// 正在执行的闭包没有提前析构。放行、join 并再次回收后 token 才能过期。
/// 下一 block 通过四帧静音前缀证明替代 provider 已被真正接管。
/// @warning 测试阻塞仅用于确定性制造并发窗口，不是生产回调允许的行为。
///
/// retiredReferenceProviderCount 在旧调用阻塞时至少为一，表示控制线程已经发布
/// 新 provider 但不能回收旧闭包。join 后计数应归零且 token 过期；随后新目标
/// 从 replacementClock 的零帧位置计算，输出前四帧静音、源位置前进十二帧。
/// weak_ptr 仅观察 token，不延长旧闭包；gate 则由测试变量继续持有以便放行。
/// 首块前八帧静音断言证明正在执行的旧 provider 结果没有被中途替换污染。
int testConcurrentProviderReplacement(
    const std::shared_ptr<ice::AudioTrack>& track)
{
    constexpr std::size_t BLOCK_FRAMES = 16U;
    constexpr std::size_t START_OFFSET = 8U;

    auto gate  = std::make_shared<ProviderGate>();
    auto token = std::make_shared<std::array<std::byte, 1U>>();
    std::weak_ptr<std::array<std::byte, 1U>> retiredToken = token;

    ice::SourceNode source(track);
    source.setvolume(1.0F);
    source.set_scheduled_start_frame(START_OFFSET);
    // token 不参与计算，只作为闭包生命周期的可观察资源。
    source.set_reference_pos_provider([gate, token]() -> std::size_t {
        static_cast<void>(token);
        gate->entered.store(true, std::memory_order_release);
        while ( !gate->release.load(std::memory_order_acquire) ) {
            std::this_thread::yield();
        }
        return 0U;
    });
    token.reset();
    source.play();

    ice::AudioBuffer buffer(ice::ICEConfig::internal_format, BLOCK_FRAMES);
    // 等待 entered 保证替换发生时旧 provider 已处于执行临界区。
    std::thread processor([&source, &buffer]() { source.process(buffer); });
    while ( !gate->entered.load(std::memory_order_acquire) ) {
        std::this_thread::yield();
    }

    ReferenceClock replacementClock;
    // 原始函数指针 provider 无闭包分配，用于验证兼容接口也能原子替换。
    source.set_reference_pos_provider(&replacementClock, readReferenceClock);
    source.reclaimRetiredReferenceProviders();

    int failures = 0;
    failures += expectTrue(!retiredToken.expired(),
                           "hazard state retains provider capture in process");
    failures += expectTrue(source.retiredReferenceProviderCount() >= 1U,
                           "active provider remains in retired queue");

    // 旧调用返回后 hazard 解除，控制线程回收才可销毁闭包捕获。
    gate->release.store(true, std::memory_order_release);
    processor.join();
    source.reclaimRetiredReferenceProviders();

    failures += expectTrue(retiredToken.expired(),
                           "provider capture is reclaimed after process");
    failures += expectTrue(source.retiredReferenceProviderCount() == 0U,
                           "provider retired queue fully reclaims");
    failures += expectTrue(isSilent(buffer, 0U, START_OFFSET),
                           "concurrent replacement preserves current block "
                           "provider result");

    // 新一轮设置更短目标，以输出静音前缀和源位置验证替代 provider 生效。
    source.set_playpos(0U);
    source.set_scheduled_start_frame(4U);
    source.play();
    replacementClock.frame.store(0U, std::memory_order_relaxed);
    source.process(buffer);
    failures += expectTrue(
        isSilent(buffer, 0U, 4U) && source.get_playpos() == BLOCK_FRAMES - 4U,
        "next block observes raw replacement provider");
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
    std::free(memory);
}

/// @brief 释放普通数组堆内存。
/// @param memory 待释放内存。
void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

/// @brief 释放带尺寸的普通单对象堆内存。
/// @param memory 待释放内存。
/// @param size 原始请求字节数。
void operator delete(void* memory, std::size_t size) noexcept
{
    static_cast<void>(size);
    std::free(memory);
}

/// @brief 释放带尺寸的普通数组堆内存。
/// @param memory 待释放内存。
/// @param size 原始请求字节数。
void operator delete[](void* memory, std::size_t size) noexcept
{
    static_cast<void>(size);
    std::free(memory);
}

/// @brief 运行 SourceNode 定时播放实时安全测试。
/// @param argc 参数数量。
/// @param argv 第一个参数为短音频测试资源。
/// @return 测试通过时返回 0。
///
/// 真实音频资源只用于提供稳定 PCM，三个场景各自构造 SourceNode，避免定时目标、
/// final 状态和 provider 退役队列互相污染。样本至少 128 帧才能覆盖参考读取。
/// 三个失败数量相加后统一返回，既保留独立诊断，也确保夹具加载失败立即停止。
/// 资源加载结束后所有测试都只读取缓存，不把后台解码时序混入实时断言。
int main(int argc, char** argv)
{
    if ( argc < 2 ) {
        XERROR("Usage: SourceNodeScheduledRealtimeTest <sample_path>");
        return 1;
    }

    // 使用生产 AudioPool 解码路径，加载工作在任何分配计数开始之前完成。
    ice::ThreadPool threadPool(2);
    ice::AudioPool  audioPool;
    auto            track =
        audioPool
            .get_or_load(threadPool, std::filesystem::path(argv[1]).string())
            .lock();
    if ( !track || track->num_frames() < 128U ) {
        XERROR("Failed to load SourceNode realtime test sample");
        return 1;
    }

    const int failures = testBlockLocalStartAndZeroAllocation(track) +
                         testSameBlockFinalInputNotification(track) +
                         testConcurrentProviderReplacement(track);
    if ( failures != 0 ) {
        XERROR("SourceNode scheduled realtime tests failed: {}", failures);
        return 1;
    }
    return 0;
}
