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
    /// @brief 当前 block 起点帧。
    std::atomic<std::size_t> frame{ 0U };
};

/// @brief 从常驻原子时钟读取参考帧。
/// @param context 指向 ReferenceClock 的非空地址。
/// @return 当前参考帧。
std::size_t readReferenceClock(const void* context) noexcept
{
    const auto* clock = static_cast<const ReferenceClock*>(context);
    return clock ? clock->frame.load(std::memory_order_relaxed) : 0U;
}

/// @brief 兼容 provider 并发测试使用的阻塞门。
struct ProviderGate {
    /// @brief provider 是否已经进入。
    std::atomic_bool entered{ false };

    /// @brief 是否允许 provider 返回。
    std::atomic_bool release{ false };
};

/// @brief 记录 SourceNode 同块输入结束通知。
struct FinalInputProbe {
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
int testBlockLocalStartAndZeroAllocation(
    const std::shared_ptr<ice::AudioTrack>& track)
{
    constexpr std::size_t BLOCK_FRAMES = 64U;
    constexpr std::size_t START_OFFSET = 32U;

    ice::SourceNode referenceSource(track);
    referenceSource.setvolume(1.0F);
    referenceSource.play();
    ice::AudioBuffer referenceBuffer(ice::ICEConfig::internal_format,
                                     BLOCK_FRAMES - START_OFFSET);
    referenceSource.process(referenceBuffer);

    ReferenceClock  clock;
    ice::SourceNode scheduledSource(track);
    scheduledSource.setvolume(1.0F);
    scheduledSource.set_playpos(0U);
    scheduledSource.set_scheduled_start_frame(START_OFFSET);
    scheduledSource.set_reference_pos_provider(&clock, readReferenceClock);
    scheduledSource.play();

    ice::AudioBuffer scheduledBuffer(ice::ICEConfig::internal_format,
                                     BLOCK_FRAMES);
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
int testSameBlockFinalInputNotification(
    const std::shared_ptr<ice::AudioTrack>& track)
{
    constexpr std::size_t BLOCK_FRAMES = 64U;
    const std::size_t     totalFrames  = track->num_frames();
    ice::SourceNode       source(track);
    FinalInputProbe       probe{ .source = &source };
    source.set_final_input_listener(&probe, recordFinalInput);
    source.setvolume(1.0F);
    source.set_playpos(totalFrames - 16U);
    source.play();

    ice::AudioBuffer buffer(ice::ICEConfig::internal_format, BLOCK_FRAMES);
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
    std::thread processor([&source, &buffer]() { source.process(buffer); });
    while ( !gate->entered.load(std::memory_order_acquire) ) {
        std::this_thread::yield();
    }

    ReferenceClock replacementClock;
    source.set_reference_pos_provider(&replacementClock, readReferenceClock);
    source.reclaimRetiredReferenceProviders();

    int failures = 0;
    failures += expectTrue(!retiredToken.expired(),
                           "hazard state retains provider capture in process");
    failures += expectTrue(source.retiredReferenceProviderCount() >= 1U,
                           "active provider remains in retired queue");

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
int main(int argc, char** argv)
{
    if ( argc < 2 ) {
        XERROR("Usage: SourceNodeScheduledRealtimeTest <sample_path>");
        return 1;
    }

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
