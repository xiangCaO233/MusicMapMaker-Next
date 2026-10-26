#pragma once

#include <cstddef>

namespace MMM::Test::AllocationProbe
{
// 三个实时测试共享观察状态，不能各自定义与替换函数不相连的私有计数。
// inline 变量跨翻译单元保持同一身份，thread_local 保持控制与处理线程隔离。
// 测试只在被测回调期间打开开关，并在记录日志与断言之前关闭。
// 该头仅服务测试目标，产品代码不得包含或依赖测试分配替换。

/// @brief 当前线程是否正在观察 C++ 堆分配与释放。
inline thread_local bool g_trackAllocations{ false };

/// @brief 当前线程观察窗口内的堆分配次数；打开窗口前由用例清零。
inline thread_local std::size_t g_allocationCount{ 0U };

/// @brief 当前线程观察窗口内的堆释放次数；不统计其他线程的控制侧回收。
inline thread_local std::size_t g_deallocationCount{ 0U };
}  // namespace MMM::Test::AllocationProbe
