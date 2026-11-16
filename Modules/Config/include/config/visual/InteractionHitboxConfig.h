#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace MMM::Config
{
/// @brief 可独立调整捕捉范围的物件部件，不包含画布布局组件。
/// @note 顺序只用于固定数组访问；持久化使用下面的具名键。
enum class HitboxComponent {
    Head,          ///< 单键、长条与滑键的头部。
    HoldBody,      ///< 长条体与滑键横向连接体。
    HoldEnd,       ///< 长条尾部。
    FlickArrow,    ///< 独立滑键和折线末端箭头。
    PolylineNode,  ///< 折线首节点和连接节点。
    SampleAnchor,  ///< 音频采样锚点。
    SampleOffset,  ///< 音频采样偏移触发点。
    Count          ///< 固定数组的元素数量，不是有效部件。
};

/// @brief 每类部件的附加捕捉倍率，绘制尺寸保持不变。
/// @note 单位倍率继承旧版全局配置，不改变现有用户的捕捉范围。
/// @details 最终范围为原始范围乘全局基础倍率，再乘本部件倍率。
/// 该值不包含显示开关，隐藏包围框时仍保持同样的捕捉行为。
/// 类型仅存两个标量，允许直接嵌入配置和跨线程快照。
struct InteractionHitboxScale {
    /// @brief 横向倍率，以原包围框中心扩展或收缩。
    float x{ 1.0F };
    /// @brief 纵向倍率，与横向独立生效。
    float y{ 1.0F };
    /// @brief 值比较仅用于配置变化判定，不把显示或临时选择状态混入范围设置。
    bool operator==(const InteractionHitboxScale&) const = default;
};

/// @brief 捕捉倍率的统一下限，禁止退化成空框。
inline constexpr float MIN_HITBOX_SCALE = 0.25F;
/// @brief 捕捉倍率的统一上限，约束误配置带来的过大范围。
inline constexpr float MAX_HITBOX_SCALE = 4.0F;
/// @brief 固定大小值数组，快照复制不分配也不引入共享所有权。
using InteractionHitboxScales =
    std::array<InteractionHitboxScale,
               static_cast<std::size_t>(HitboxComponent::Count)>;

/// @brief 配置文件中的稳定键，禁止将本地化文字写入配置。
inline constexpr std::array<std::string_view,
                            static_cast<std::size_t>(HitboxComponent::Count)>
    HITBOX_COMPONENT_KEYS{ "head",        "holdBody",     "holdEnd",
                           "flickArrow",  "polylineNode", "sampleAnchor",
                           "sampleOffset" };

/// @brief 统一读取端和快照发布端的倍率约束。
/// @return 有限值夹取到允许范围，非法数值回退到单位倍率。
/// @warning 每帧快照准备时调用，只做常量级算术，不分配或加锁。
[[nodiscard]] inline float sanitizeHitboxScale(float value) noexcept
{
    // 先排除 NaN 和无穷，避免夹取传播不可比较值。
    return std::isfinite(value)
               ? std::clamp(value, MIN_HITBOX_SCALE, MAX_HITBOX_SCALE)
               : 1.0F;
}
}  // namespace MMM::Config
