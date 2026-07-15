#include "config/FrameLimitUtils.h"

#include <cmath>

namespace
{
/// @brief 判断两个频率或时间间隔是否近似相等。
/// @param lhs 左侧数值。
/// @param rhs 右侧数值。
/// @return 误差在测试容限内时返回 true。
bool approximatelyEqual(double lhs, double rhs)
{
    return std::abs(lhs - rhs) <= 1e-9;
}
}  // namespace

/// @brief 覆盖 VSync、多倍刷新率、无限制及无效刷新率回退换算。
/// @return 所有断言通过时返回 0。
int main()
{
    using MMM::Config::FrameLimitPreference;
    using MMM::Config::frameLimitTargetInterval;
    using MMM::Config::frameLimitTargetRate;

    bool ok = true;
    ok &= approximatelyEqual(
        frameLimitTargetRate(FrameLimitPreference::VSync, 165), 165.0);
    ok &= approximatelyEqual(
        frameLimitTargetRate(FrameLimitPreference::Refresh2x, 165), 330.0);
    ok &= approximatelyEqual(
        frameLimitTargetRate(FrameLimitPreference::Refresh4x, 165), 660.0);
    ok &= approximatelyEqual(
        frameLimitTargetRate(FrameLimitPreference::Refresh8x, 165), 1320.0);
    ok &= approximatelyEqual(
        frameLimitTargetRate(FrameLimitPreference::Unlimited, 165), 0.0);
    ok &= approximatelyEqual(
        frameLimitTargetRate(FrameLimitPreference::VSync, 0), 60.0);
    ok &= approximatelyEqual(
        frameLimitTargetInterval(FrameLimitPreference::VSync, 165),
        1.0 / 165.0);
    ok &= approximatelyEqual(
        frameLimitTargetInterval(FrameLimitPreference::Unlimited, 165), 0.0);
    return ok ? 0 : 1;
}
