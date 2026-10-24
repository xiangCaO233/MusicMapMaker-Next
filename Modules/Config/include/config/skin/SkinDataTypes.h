#pragma once

#include "config/skin/SkinTypes.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMM::Config
{
/// @brief 皮肤亮暗外观的默认主题映射。
struct SkinThemeBinding {
    /// @brief 系统为亮色或无法识别系统偏好时使用的主题。
    std::string light{ "DeepDark" };

    /// @brief 系统为暗色时使用的主题。
    std::string dark{ "DeepDark" };
};

/// @brief 动画序列帧及其纹理起点，容器保持独立值语义。
struct SkinEffectSequence {
    /// @brief 按播放顺序保存帧路径，加载后由图集分配纹理标识。
    std::vector<std::filesystem::path> frames;
    /// @brief 第一帧的纹理标识，后续帧按既有连续编号访问。
    uint32_t startId{ 0 };  // 对应的起始 TextureID
    /// @brief 非预乘贴图按 Alpha 加权后加亮背景，默认保持普通覆盖。
    bool additiveBlend{ false };
};

/// @brief 打击和发光效果的默认参数。
struct SkinEffectsConfig {
    /// @brief 打击特效纹理布局配置。
    struct HitEffect {
        /// @brief 旧皮肤默认保持固定位置、固定尺寸绘制。
        HitEffectLayoutMode layout{ HitEffectLayoutMode::Fixed };
    } hitEffect;

    /// @brief 发光后处理参数，使用历史默认值。
    struct Glow {
        /// @brief 发光处理轮次。
        int passes = 8;
        /// @brief 发光叠加强度。
        float intensity = 1.0f;
    } glow;
};

/// @brief 命名画布及其着色器资产映射。
struct SkinCanvasConfig {
    /// @brief 画布名称
    std::string canvas_name{};
    /// @brief 画布着色器模块表 (Key: 资产ID, Value: 文件路径)
    std::unordered_map<std::string, std::filesystem::path>
        canvas_shader_modules{};
};
}  // namespace MMM::Config
