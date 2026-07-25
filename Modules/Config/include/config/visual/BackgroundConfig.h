#pragma once

#include <nlohmann/json_fwd.hpp>

namespace MMM::Config
{

/// @brief 纹理在目标矩形中的填充方式。
enum class BackgroundFillMode {
    Stretch,
    AspectFit,
    AspectFill,
    Center,
};

/// @brief 背景实时频谱允许的最少单声道频段数。
inline constexpr int BACKGROUND_SPECTRUM_MIN_BANDS = 10;
/// @brief 背景实时频谱允许的最多单声道频段数。
inline constexpr int BACKGROUND_SPECTRUM_MAX_BANDS = 128;

/// @brief 位于背景资源之上的实时立体声音频频谱配置。
struct BackgroundSpectrumConfig {
    /// @brief 是否显示背景频谱。
    bool enabled{ false };
    /// @brief 每个声道从画布中心向外绘制的频段数。
    int bandCount{ 24 };
    /// @brief 频谱占画布宽度的比例。
    float widthRatio{ 1.0f };
    /// @brief 频谱最大高度占画布高度的比例。
    float heightRatio{ 0.35f };
    /// @brief 频谱底边相对画布高度的位置比例。
    float baselineRatio{ 1.0f };
    /// @brief 频谱整体不透明度。
    float opacity{ 0.35f };
    /// @brief 是否将实际播放的 HitEffect 音效计入频谱。
    bool includeHitEffects{ true };
};

/// @brief 背景图像与暗化层配置。
struct BackgroundConfig {
    /// @brief 填充模式。
    BackgroundFillMode fillMode{ BackgroundFillMode::AspectFill };
    /// @brief 背景暗化比例；0 表示不暗化，1 表示完全暗化。
    float darken_ratio{ 0.7f };
    /// @brief 背景不透明度；0 表示完全透明。
    float opaque_ratio{ 1.0f };
    /// @brief 背景图片上方的实时立体声音频频谱。
    BackgroundSpectrumConfig spectrum;
};

/// @brief 将背景填充方式序列化为 JSON。
void to_json(nlohmann::json& json, const BackgroundFillMode& mode);
/// @brief 从 JSON 读取背景填充方式。
void from_json(const nlohmann::json& json, BackgroundFillMode& mode);
/// @brief 将背景频谱配置序列化为 JSON。
void to_json(nlohmann::json& json, const BackgroundSpectrumConfig& config);
/// @brief 从 JSON 读取并规整背景频谱配置。
void from_json(const nlohmann::json& json, BackgroundSpectrumConfig& config);
/// @brief 将背景配置序列化为 JSON。
void to_json(nlohmann::json& json, const BackgroundConfig& config);
/// @brief 从 JSON 读取背景配置。
void from_json(const nlohmann::json& json, BackgroundConfig& config);

}  // namespace MMM::Config
