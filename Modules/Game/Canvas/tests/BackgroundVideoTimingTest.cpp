#include "canvas/BackgroundVideoTiming.h"

#include <cmath>

namespace
{

/// @brief 比较两个时间值是否足够接近。
bool near(double value, double expected)
{
    return std::abs(value - expected) < 1e-9;
}

/// @brief 验证调用方解析后的暂停时间按原值映射到视频。
bool testPausedClock()
{
    return near(MMM::Canvas::calculateBackgroundVideoTime(10.0, 2.0), 8.0);
}

/// @brief 验证调用方完成亚帧外推后的时间映射到视频。
bool testPlayingClock()
{
    return near(MMM::Canvas::calculateBackgroundVideoTime(10.1, 2.0), 8.1);
}

/// @brief 验证调用方拒绝过时外推后的时间按原值映射到视频。
bool testStaleSnapshot()
{
    return near(MMM::Canvas::calculateBackgroundVideoTime(10.0, 2.0), 8.0);
}

/// @brief 验证正开始偏移与负开始偏移的时间语义。
bool testStartOffsets()
{
    return near(MMM::Canvas::calculateBackgroundVideoTime(1.0, 2.0), -1.0) &&
           near(MMM::Canvas::calculateBackgroundVideoTime(1.0, -2.0), 3.0);
}

/// @brief 验证调用方钳制的谱面末尾时间按原值映射到视频。
bool testFinalTailStopsAtTimelineEnd()
{
    return near(MMM::Canvas::calculateBackgroundVideoTime(20.0, 2.0), 18.0);
}

}  // namespace

/// @brief 覆盖背景视频与谱面播放时钟的同步计算。
/// @return 所有检查通过时返回 0。
int main()
{
    return testPausedClock() && testPlayingClock() && testStaleSnapshot() &&
                   testStartOffsets() && testFinalTailStopsAtTimelineEnd()
               ? 0
               : 1;
}
