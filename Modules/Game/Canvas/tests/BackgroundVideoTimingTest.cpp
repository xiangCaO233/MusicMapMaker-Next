#include "canvas/BackgroundVideoTiming.h"

#include <cmath>

namespace
{

/// @brief 比较两个时间值是否足够接近。
bool near(double value, double expected)
{
    return std::abs(value - expected) < 1e-9;
}

/// @brief 验证暂停时不会使用系统时钟外推视频。
bool testPausedClock()
{
    return near(MMM::Canvas::calculateBackgroundVideoTime(
                    10.0, 2.0, false, 100.0, 2.0, 100.05),
                8.0);
}

/// @brief 验证播放时按倍速进行短时间亚帧外推。
bool testPlayingClock()
{
    return near(MMM::Canvas::calculateBackgroundVideoTime(
                    10.0, 2.0, true, 100.0, 2.0, 100.05),
                8.1);
}

/// @brief 验证过时快照不外推，避免视频时钟过度超前。
bool testStaleSnapshot()
{
    return near(MMM::Canvas::calculateBackgroundVideoTime(
                    10.0, 2.0, true, 100.0, 4.0, 100.2),
                8.0);
}

/// @brief 验证正开始偏移与负开始偏移的时间语义。
bool testStartOffsets()
{
    return near(MMM::Canvas::calculateBackgroundVideoTime(
                    1.0, 2.0, false, 0.0, 1.0, 0.0),
                -1.0) &&
           near(MMM::Canvas::calculateBackgroundVideoTime(
                    1.0, -2.0, false, 0.0, 1.0, 0.0),
                3.0);
}

}  // namespace

/// @brief 覆盖背景视频与谱面播放时钟的同步计算。
/// @return 所有检查通过时返回 0。
int main()
{
    return testPausedClock() && testPlayingClock() && testStaleSnapshot() &&
                   testStartOffsets()
               ? 0
               : 1;
}
