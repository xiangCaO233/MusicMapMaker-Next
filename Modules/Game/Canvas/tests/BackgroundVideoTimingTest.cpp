#include "canvas/BackgroundVideoTiming.h"

#include <cmath>

namespace
{

/// @brief 比较两个时间值是否足够接近。
/// @param value 实际视频时间。
/// @param expected 期望视频时间。
/// @return 差值小于时间测试容差时返回 true。
bool near(double value, double expected)
{
    return std::abs(value - expected) < 1e-9;
}

/// @brief 验证调用方解析后的暂停时间按原值映射到视频。
bool testPausedClock()
{
    // 暂停状态由调用方解析，本函数只扣除视频开始偏移。
    return near(MMM::Canvas::calculateBackgroundVideoTime(10.0, 2.0), 8.0);
}

/// @brief 验证调用方完成亚帧外推后的时间映射到视频。
bool testPlayingClock()
{
    // 亚帧推进后的 0.1 秒必须原样保留到视频时间。
    return near(MMM::Canvas::calculateBackgroundVideoTime(10.1, 2.0), 8.1);
}

/// @brief 验证调用方拒绝过时外推后的时间按原值映射到视频。
bool testStaleSnapshot()
{
    // 过时快照回退已在调用方完成，此处不再进行第二次时钟判断。
    return near(MMM::Canvas::calculateBackgroundVideoTime(10.0, 2.0), 8.0);
}

/// @brief 验证正开始偏移与负开始偏移的时间语义。
bool testStartOffsets()
{
    // 尚未到开始点返回负值；负开始偏移则让视频时间领先谱面。
    return near(MMM::Canvas::calculateBackgroundVideoTime(1.0, 2.0), -1.0) &&
           near(MMM::Canvas::calculateBackgroundVideoTime(1.0, -2.0), 3.0);
}

/// @brief 验证调用方钳制的谱面末尾时间按原值映射到视频。
bool testFinalTailStopsAtTimelineEnd()
{
    // 谱面末尾钳制值按普通时间处理，结果不应继续向后外推。
    return near(MMM::Canvas::calculateBackgroundVideoTime(20.0, 2.0), 18.0);
}

}  // namespace

/// @brief 覆盖背景视频与谱面播放时钟的同步计算。
/// @return 所有检查通过时返回 0。
int main()
{
    // 五个用例隔离调用方时钟解析后的纯减法语义。
    return testPausedClock() && testPlayingClock() && testStaleSnapshot() &&
                   testStartOffsets() && testFinalTailStopsAtTimelineEnd()
               ? 0
               : 1;
}
