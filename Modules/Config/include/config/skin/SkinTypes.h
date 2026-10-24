#pragma once

#include <cstdint>

namespace MMM::Config
{
/// @brief 皮肤推荐 UI 主题对应的系统外观分支。
/// @note 保持 uint8_t 底层类型及原枚举顺序，供外观映射直接比较。
enum class SkinThemeAppearance : std::uint8_t {
    Light,  ///< 系统偏好亮色外观。
    Dark    ///< 系统偏好暗色外观。
};

/// @brief 打击特效纹理在轨道中的布局方式。
/// @note 渲染配置仅保存此值；该头不依赖皮肤管理器或资源容器。
enum class HitEffectLayoutMode : std::uint8_t {
    Fixed,     ///< 在判定线打击点按物件缩放绘制固定尺寸纹理。
    TrackFill  ///< 将纹理拉伸至对应单轨的完整可见区域。
};

/// @brief 皮肤未声明序列帧速率时使用的默认播放帧率。
/// @note 加载器与默认 DTO 共用同一常量，避免缺省值在拆分后分歧。
inline constexpr float DEFAULT_EFFECT_BASE_FPS = 120.0F;

/// @brief 皮肤原始 RGBA 浮点通道，按值传递且不执行颜色空间转换。
struct Color {
    /// @brief 红、绿、蓝和透明度；保持原始顺序及未指定初值的语义。
    float r, g, b, a;
};


}  // namespace MMM::Config
