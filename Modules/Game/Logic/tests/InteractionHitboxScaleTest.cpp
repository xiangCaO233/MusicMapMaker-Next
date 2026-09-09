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
/// @note 使用绝对坐标容差；实体标识、部位和子节点索引仍要求精确相等。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-6F;
}

/// @brief 验证横纵缩放围绕包围盒中心展开且保留拾取元数据。
/// @return 几何与元数据均符合预期时返回 true。
bool testCenteredScale()
{
    // 矩形故意不是正方形，横纵倍率也不同，以检测宽高或倍率混用。
    // 实体仅作为元数据哨兵，不需要创建注册表或真实音符。
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
    // 原中心为 (105,210)，放大后左上角必须向左上移动而非固定不变。
    const auto scaled = MMM::Logic::scaleInteractionHitbox(source, 3.0F, 2.0F);
    // 宽增加 20、高增加 20，左上角各减 10 才能保持中心不变。
    // 同一断言同时检查拾取身份，避免几何正确却把长条体误识别成头部。
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
/// @note 只覆盖负值和 NaN，不将本例当作所有非法浮点输入的完整测试。
bool testInvalidScaleFallback()
{
    // 使用不同于几何用例的起点尺寸，避免实现中的固定默认框恰好通过。
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
    // 横向给负倍率，纵向给 NaN，分别覆盖数值范围错误和非有限值。
    // 回退应是单位倍率，而不是缩成零面积或翻转命中范围。
    const auto scaled = MMM::Logic::scaleInteractionHitbox(
        source, -2.0F, std::numeric_limits<float>::quiet_NaN());
    // 位置与大小全部保持，单独检查宽高会漏掉回退时意外偏移中心的问题。
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
/// @note 这是包围盒变换测试，不运行鼠标拾取或真实图形绘制。
int main()
{
    // 两个用例独立构造只读输入，不依赖皮肤、相机状态或外部资源。
    // 先验收合法缩放，再检查非法倍率回退，保持现有短路失败顺序。
    // 非零退出码用于回归运行器，具体几何错误由用例日志定位。
    return testCenteredScale() && testInvalidScaleFallback() ? 0 : 1;
}
