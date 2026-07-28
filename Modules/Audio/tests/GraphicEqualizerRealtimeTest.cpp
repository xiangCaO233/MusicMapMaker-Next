#include "log/colorful-log.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ice/core/IAudioNode.hpp>
#include <ice/core/effect/GraphicEqualizer.hpp>
#include <ice/manage/AudioBuffer.hpp>
#include <memory>
#include <new>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
/// @brief 当前线程是否正在统计普通 C++ 堆操作。
thread_local bool g_trackAllocations{ false };

/// @brief 当前统计区间内发生的 C++ 堆分配次数。
thread_local std::size_t g_allocationCount{ 0U };

/// @brief 当前统计区间内发生的 C++ 堆释放次数。
thread_local std::size_t g_deallocationCount{ 0U };

/// @brief 记录断言失败并返回可累计的失败数量。
/// @param condition 需要成立的条件。
/// @param label 失败时输出的测试标签。
/// @return 条件成立时返回 0，否则返回 1。
int expectTrue(bool condition, std::string_view label)
{
    if ( condition ) return 0;
    XERROR("GraphicEqualizer realtime assertion failed: {}", label);
    return 1;
}

/// @brief 写入固定周期波形的无分配输入节点。
class TestSignalNode final : public ice::IAudioNode
{
public:
    /// @brief 将固定周期波形写入整个输出 block。
    /// @param buffer 输出缓冲区。
    /// @warning 测试音频热路径：不得分配内存或获取锁。
    void process(ice::AudioBuffer& buffer) override
    {
        static constexpr std::array<float, 8U> SIGNAL{ 0.0F,  0.25F, 0.5F,
                                                       0.25F, 0.0F,  -0.25F,
                                                       -0.5F, -0.25F };
        float**                                samples = buffer.raw_ptrs();
        if ( !samples ) return;

        for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
              ++channel ) {
            for ( std::size_t frame = 0U; frame < buffer.num_frames();
                  ++frame ) {
                samples[channel][frame] = SIGNAL[frame % SIGNAL.size()];
            }
        }
    }
};

/// @brief 检查一个缓冲区是否只包含有限采样。
/// @param buffer 待检查缓冲区。
/// @return 全部采样有限时返回 true。
bool allSamplesFinite(const ice::AudioBuffer& buffer)
{
    const float* const* samples = buffer.raw_ptrs();
    if ( !samples ) return buffer.num_frames() == 0U;
    for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < buffer.num_frames(); ++frame ) {
            if ( !std::isfinite(samples[channel][frame]) ) return false;
        }
    }
    return true;
}

/// @brief 检查一个缓冲区是否全部为静音。
/// @param buffer 待检查缓冲区。
/// @return 全部采样为零时返回 true。
bool allSamplesSilent(const ice::AudioBuffer& buffer)
{
    const float* const* samples = buffer.raw_ptrs();
    if ( !samples ) return buffer.num_frames() == 0U;
    for ( std::uint16_t channel = 0U; channel < buffer.num_channels();
          ++channel ) {
        for ( std::size_t frame = 0U; frame < buffer.num_frames(); ++frame ) {
            if ( samples[channel][frame] != 0.0F ) return false;
        }
    }
    return true;
}

/// @brief 验证预备后的单线程音频回调不发生 C++ 堆操作。
/// @return 失败断言数量。
int testPreparedCallbackHasNoHeapOperations()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 64U;

    auto                  input = std::make_shared<TestSignalNode>();
    ice::GraphicEqualizer equalizer({ 125.0, 1000.0, 8000.0 });
    equalizer.prepare(FORMAT, BLOCK_FRAMES);
    equalizer.set_inputnode(input);
    equalizer.set_band_gain_db(1U, 6.0F);

    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    equalizer.process(output);
    g_trackAllocations = false;

    int failures = 0;
    failures += expectTrue(g_allocationCount == 0U,
                           "prepared callback performs no C++ heap allocation");
    failures +=
        expectTrue(g_deallocationCount == 0U,
                   "prepared callback performs no C++ heap deallocation");
    failures += expectTrue(allSamplesFinite(output),
                           "prepared callback produces finite samples");

    ice::AudioBuffer oversized(FORMAT, BLOCK_FRAMES + 1U);
    g_allocationCount   = 0U;
    g_deallocationCount = 0U;
    g_trackAllocations  = true;
    equalizer.process(oversized);
    g_trackAllocations = false;
    failures += expectTrue(g_allocationCount == 0U && g_deallocationCount == 0U,
                           "oversized callback rejects without heap activity");
    failures += expectTrue(allSamplesSilent(oversized),
                           "oversized callback returns deterministic silence");
    return failures;
}

/// @brief 验证控制线程更新参数时音频线程只在 block 边界切换稳定状态。
/// @return 失败断言数量。
int testConcurrentParameterPublication()
{
    constexpr ice::AudioDataFormat FORMAT{
        .channels   = 2U,
        .samplerate = 48000U,
    };
    constexpr std::size_t BLOCK_FRAMES = 128U;
    constexpr std::size_t UPDATE_COUNT = 2000U;

    auto                  input = std::make_shared<TestSignalNode>();
    ice::GraphicEqualizer equalizer({ 250.0, 1000.0, 4000.0 });
    equalizer.prepare(FORMAT, BLOCK_FRAMES);
    equalizer.set_inputnode(input);

    std::atomic_bool   start{ false };
    std::atomic_bool   samplesFinite{ true };
    std::atomic_size_t callbackAllocations{ 0U };
    std::atomic_size_t callbackDeallocations{ 0U };

    std::thread processor([&]() {
        ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);
        while ( !start.load(std::memory_order_acquire) ) {
            std::this_thread::yield();
        }

        g_allocationCount   = 0U;
        g_deallocationCount = 0U;
        g_trackAllocations  = true;
        for ( std::size_t update = 0U; update < UPDATE_COUNT; ++update ) {
            equalizer.process(output);
            if ( !allSamplesFinite(output) ) {
                samplesFinite.store(false, std::memory_order_release);
                break;
            }
        }
        g_trackAllocations = false;
        callbackAllocations.store(g_allocationCount, std::memory_order_release);
        callbackDeallocations.store(g_deallocationCount,
                                    std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    bool controlsRemainValid = true;
    for ( std::size_t update = 0U; update < UPDATE_COUNT; ++update ) {
        const std::size_t bandIndex = update % equalizer.get_band_count();
        const float       gain =
            static_cast<float>(static_cast<int>(update % 25U) - 12);
        const float q = 0.7F + static_cast<float>(update % 12U) * 0.1F;
        equalizer.set_band_gain_db(bandIndex, gain);
        equalizer.set_band_q_factor(bandIndex, q);

        const double response = equalizer.get_total_magnitude_response(1000.0);
        controlsRemainValid =
            controlsRemainValid && std::isfinite(response) && response > 0.0 &&
            std::isfinite(equalizer.get_band_gain_db(bandIndex)) &&
            equalizer.get_band_q_factor(bandIndex) > 0.0;
    }
    processor.join();
    equalizer.reclaim_retired_filter_states();

    int failures = 0;
    failures += expectTrue(
        callbackAllocations.load(std::memory_order_acquire) == 0U,
        "concurrent parameter publication does not allocate in callback");
    failures += expectTrue(
        callbackDeallocations.load(std::memory_order_acquire) == 0U,
        "concurrent parameter publication does not free in callback");
    failures += expectTrue(samplesFinite.load(std::memory_order_acquire),
                           "concurrent states always produce finite samples");
    failures += expectTrue(controlsRemainValid,
                           "control getters and response remain race free");
    failures += expectTrue(equalizer.retired_filter_state_count() == 0U,
                           "control thread reclaims retired states after use");
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

/// @brief 运行 GraphicEqualizer 实时安全和并发发布测试。
/// @return 测试通过时返回 0。
int main()
{
    const int failures = testPreparedCallbackHasNoHeapOperations() +
                         testConcurrentParameterPublication();
    if ( failures != 0 ) {
        XERROR("GraphicEqualizer realtime tests failed: {}", failures);
        return 1;
    }
    return 0;
}
