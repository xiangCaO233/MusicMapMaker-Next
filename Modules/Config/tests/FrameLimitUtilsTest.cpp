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
    // 换算仅含少量乘除法，固定绝对误差足以覆盖测试中的确定输入。
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

    // 聚合结果让全部分支都被执行，失败时统一返回非零状态给 CTest。
    bool ok = true;
    // 基础 VSync 应原样采用设备刷新率。
    ok &= approximatelyEqual(
        frameLimitTargetRate(FrameLimitPreference::VSync, 165), 165.0);
    // 多倍档位分别验证倍率映射，不依赖相邻枚举的数值布局。
    ok &= approximatelyEqual(
        frameLimitTargetRate(FrameLimitPreference::Refresh2x, 165), 330.0);
    ok &= approximatelyEqual(
        frameLimitTargetRate(FrameLimitPreference::Refresh4x, 165), 660.0);
    ok &= approximatelyEqual(
        frameLimitTargetRate(FrameLimitPreference::Refresh8x, 165), 1320.0);
    // Unlimited 使用零作为“不限频”哨兵，而不是极大目标值。
    ok &= approximatelyEqual(
        frameLimitTargetRate(FrameLimitPreference::Unlimited, 165), 0.0);
    ok &= approximatelyEqual(
        frameLimitTargetRate(FrameLimitPreference::VSync, 0), 60.0);
    // 零刷新率代表平台查询失败，必须采用公开的 60 Hz 回退约定。
    // 时间间隔必须是目标频率的倒数，供循环限频器直接使用。
    ok &= approximatelyEqual(
        frameLimitTargetInterval(FrameLimitPreference::VSync, 165),
        1.0 / 165.0);
    // Unlimited 的间隔同样保持零，避免调用方发生除零。
    ok &= approximatelyEqual(
        frameLimitTargetInterval(FrameLimitPreference::Unlimited, 165), 0.0);
    // 返回码保持简单，任何一个模式回归都会使测试失败。
    return ok ? 0 : 1;
}
