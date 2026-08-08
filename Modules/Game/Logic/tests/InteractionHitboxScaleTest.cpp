#include "logic/BeatmapSyncBuffer.h"

#include "log/colorful-log.h"

#include <cmath>
#include <limits>

namespace
{
/// @brief 使用小容差比较包围盒浮点坐标。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-6F;
}

/// @brief 验证横纵缩放围绕包围盒中心展开且保留拾取元数据。
/// @return 几何与元数据均符合预期时返回 true。
bool testCenteredScale()
{
    const MMM::Logic::Hitbox source{
        static_cast<entt::entity>(42),
        MMM::Logic::HoverPart::HoldBody,
        3,
        100.0F,
        200.0F,
        10.0F,
        20.0F,
        MMM::Logic::ChartObjectKind::PlayerNote,
    };
    const auto scaled = MMM::Logic::scaleInteractionHitbox(source, 3.0F, 2.0F);
    if ( !near(scaled.x, 90.0F) || !near(scaled.y, 190.0F) ||
         !near(scaled.w, 30.0F) || !near(scaled.h, 40.0F) ||
         scaled.entity != source.entity || scaled.part != source.part ||
         scaled.subIndex != source.subIndex || scaled.kind != source.kind ) {
        XERROR("Interaction hitbox did not scale around its center");
        return false;
    }
    return true;
}

/// @brief 验证无效缩放不会破坏原始拾取区域。
/// @return 非法横纵缩放均回退为 1 时返回 true。
bool testInvalidScaleFallback()
{
    const MMM::Logic::Hitbox source{
        static_cast<entt::entity>(7),
        MMM::Logic::HoverPart::Head,
        -1,
        12.0F,
        24.0F,
        30.0F,
        40.0F,
        MMM::Logic::ChartObjectKind::PlayerNote,
    };
    const auto scaled = MMM::Logic::scaleInteractionHitbox(
        source, -2.0F, std::numeric_limits<float>::quiet_NaN());
    if ( !near(scaled.x, source.x) || !near(scaled.y, source.y) ||
         !near(scaled.w, source.w) || !near(scaled.h, source.h) ) {
        XERROR("Invalid interaction hitbox scale did not fall back to one");
        return false;
    }
    return true;
}
}  // namespace

/// @brief 覆盖交互拾取包围盒缩放的中心保持与非法值回退。
/// @return 全部断言通过时返回 0。
int main()
{
    return testCenteredScale() && testInvalidScaleFallback() ? 0 : 1;
}
