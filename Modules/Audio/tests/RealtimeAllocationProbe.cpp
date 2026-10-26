#include "RealtimeAllocationProbe.h"

#include <cstdlib>
#include <new>

// 全局分配替换在音频套件中只编译一次，所有用例读取同一份线程局部计数。
// 默认关闭观察，控制线程的正常分配和其他音频用例不会进入计数窗口。
// 覆盖普通、数组、带尺寸和对齐重载，保留实时回调的原有检测范围。

/// @brief 为回调零分配测试代理普通单对象堆分配。
/// @param size 请求字节数。
/// @return 已分配内存。
void* operator new(std::size_t size)
{
    if ( MMM::Test::AllocationProbe::g_trackAllocations )
        ++MMM::Test::AllocationProbe::g_allocationCount;
    if ( void* memory = std::malloc(size) ) return memory;
    std::abort();
}

/// @brief 为回调零分配测试代理普通数组堆分配。
/// @param size 请求字节数。
/// @return 已分配内存。
void* operator new[](std::size_t size)
{
    if ( MMM::Test::AllocationProbe::g_trackAllocations )
        ++MMM::Test::AllocationProbe::g_allocationCount;
    if ( void* memory = std::malloc(size) ) return memory;
    std::abort();
}

/// @brief 释放普通单对象堆内存。
/// @param memory 待释放内存。
void operator delete(void* memory) noexcept
{
    if ( MMM::Test::AllocationProbe::g_trackAllocations )
        ++MMM::Test::AllocationProbe::g_deallocationCount;
    std::free(memory);
}

/// @brief 释放普通数组堆内存。
/// @param memory 待释放内存。
void operator delete[](void* memory) noexcept
{
    if ( MMM::Test::AllocationProbe::g_trackAllocations )
        ++MMM::Test::AllocationProbe::g_deallocationCount;
    std::free(memory);
}

/// @brief 释放带尺寸的普通单对象堆内存。
/// @param memory 待释放内存。
/// @param size 原始请求字节数。
void operator delete(void* memory, std::size_t size) noexcept
{
    static_cast<void>(size);
    if ( MMM::Test::AllocationProbe::g_trackAllocations )
        ++MMM::Test::AllocationProbe::g_deallocationCount;
    std::free(memory);
}

/// @brief 释放带尺寸的普通数组堆内存。
/// @param memory 待释放内存。
/// @param size 原始请求字节数。
void operator delete[](void* memory, std::size_t size) noexcept
{
    static_cast<void>(size);
    if ( MMM::Test::AllocationProbe::g_trackAllocations )
        ++MMM::Test::AllocationProbe::g_deallocationCount;
    std::free(memory);
}

/// @brief 为回调零分配测试代理对齐单对象堆分配。
/// @param size 请求字节数。
/// @param alignment 请求对齐。
/// @return 已分配内存。
void* operator new(std::size_t size, std::align_val_t alignment)
{
    if ( MMM::Test::AllocationProbe::g_trackAllocations )
        ++MMM::Test::AllocationProbe::g_allocationCount;
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
    if ( MMM::Test::AllocationProbe::g_trackAllocations )
        ++MMM::Test::AllocationProbe::g_deallocationCount;
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
