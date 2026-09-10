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
// 本测试通过覆盖全局 new/delete 观察 GraphicEqualizer 的实时行为：
//
// - prepare 之前允许分配滤波状态和 scratch；
// - 计数开关只在 equalizer.process 调用区间内启用；
// - 正常尺寸 block 不得发生普通、数组或对齐堆分配与释放；
// - 超出 prepare 容量的 block 必须拒绝并返回确定静音，不能临时扩容；
// - 控制线程并发发布增益和 Q 时，音频线程只消费完整滤波状态；
// - 旧状态由控制线程回收，不能在音频回调中析构。
//
// 分配探针采用 thread_local，主线程合法的参数状态构造不会污染 processor 线程
// 的实时计数。所有重载最终使用 malloc/free，只改变观测计数，不改变常规分配
// 语义。测试不要求某个 block 对应某次参数值，只验证发布始终完整且有界。
// processor 结束后才检查退役队列，确保控制线程承担状态析构而非音频线程。

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
        // 固定八帧周期包含正负样本与零点，可暴露滤波器产生的非有限状态。
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
    // 空数据只在零帧缓冲下合法，非空 block 必须提供声道指针。
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
    // 拒绝路径要求精确清零，而不是仅保证有限或接近零。
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
///
/// 三频段 EQ 在计数开始前完成构造、prepare、接线和参数发布。首个正常 block
/// 同时检查零分配与有限输出；随后用容量加一的 block 验证拒绝路径也不分配，
/// 且完整缓冲为静音，避免保留调用方传入的旧数据。
/// 正常输出只断言有限值，不绑定具体滤波响应；频响正确性由非实时单元测试负责，
/// 本场景专注 prepare 容量与回调内存行为。
/// oversized 输出在调用前由 AudioBuffer 构造，process
/// 必须主动清零其全部活动帧。
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
    // 非零增益确保回调真正使用已发布滤波状态，而不是旁路所有处理。
    equalizer.set_band_gain_db(1U, 6.0F);

    ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);
    // 只包围被测 process，输出缓冲构造等测试准备成本不计入实时结果。
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

    // 容量错误必须走有界失败路径，不能通过 resize 掩盖未正确 prepare 的调用。
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
///
/// processor 线程连续执行 2000 个 block，主线程同时轮换频段、增益和 Q。测试
/// 不固定哪个 block 看见哪次更新，只要求每次输出有限、控制查询有效、回调零
/// 分配零释放，并在 join 后由控制线程清空所有退役滤波状态。
/// UPDATE_COUNT 足以反复覆盖三个频段与全部测试参数，但不把执行时间作为断言。
/// response 与 getter 在控制线程同步查询，验证发布同时支持安全的诊断读取。
///
/// 增益覆盖 -12 dB 到 +12 dB，Q 覆盖 0.7 到 1.8；均为合法但变化明显的值。
/// 主线程每轮同时设置增益与 Q，允许音频线程在两次独立发布之间处理 block，
/// 但每份内部滤波器状态本身必须完整，不得出现撕裂系数或悬空指针。
/// 最终 retired_filter_state_count 为零，证明控制侧显式回收覆盖全部发布历史。
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

    // 原子结果只从 processor 写、主线程 join 后读，release/acquire
    // 明确发布边界。
    std::atomic_bool   start{ false };
    std::atomic_bool   samplesFinite{ true };
    std::atomic_size_t callbackAllocations{ 0U };
    std::atomic_size_t callbackDeallocations{ 0U };

    // 输出缓冲在计数开始前构造，线程启动门保证参数更新与 process 明确重叠。
    std::thread processor([&]() {
        ice::AudioBuffer output(FORMAT, BLOCK_FRAMES);
        while ( !start.load(std::memory_order_acquire) ) {
            std::this_thread::yield();
        }

        g_allocationCount   = 0U;
        g_deallocationCount = 0U;
        g_trackAllocations  = true;
        // 每个 block 都复查有限值，捕获读到撕裂系数造成的 NaN 或无穷。
        for ( std::size_t update = 0U; update < UPDATE_COUNT; ++update ) {
            equalizer.process(output);
            if ( !allSamplesFinite(output) ) {
                samplesFinite.store(false, std::memory_order_release);
                break;
            }
        }
        g_trackAllocations = false;
        // 离开计数区间后一次性发布线程局部结果，避免每个 block 额外原子竞争。
        callbackAllocations.store(g_allocationCount, std::memory_order_release);
        callbackDeallocations.store(g_deallocationCount,
                                    std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    bool controlsRemainValid = true;
    // 控制值覆盖正负增益及多个合法 Q，持续触发不可变状态发布与替换。
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
    // 音频线程退出后再从控制线程回收，避免测试本身违反生命周期协议。
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
///
/// 两个场景分别覆盖确定性单线程容量契约与控制/音频双线程发布协议；失败数量
/// 累加后统一返回，便于一次运行保留两个职责的诊断信息。
/// 全局分配代理仅链接进本测试可执行文件，不改变应用或其他测试的分配行为。
/// 任一断言失败都通过 XERROR 给出职责标签，返回码只表达整体通过与否。
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
