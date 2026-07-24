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

/// @brief 背景图像与暗化层配置。
struct BackgroundConfig {
    /// @brief 填充模式。
    BackgroundFillMode fillMode{ BackgroundFillMode::AspectFill };
    /// @brief 背景暗化比例；0 表示不暗化，1 表示完全暗化。
    float darken_ratio{ 0.7f };
    /// @brief 背景不透明度；0 表示完全透明。
    float opaque_ratio{ 1.0f };
};

/// @brief 将背景填充方式序列化为 JSON。
void to_json(nlohmann::json& json, const BackgroundFillMode& mode);
/// @brief 从 JSON 读取背景填充方式。
void from_json(const nlohmann::json& json, BackgroundFillMode& mode);
/// @brief 将背景配置序列化为 JSON。
void to_json(nlohmann::json& json, const BackgroundConfig& config);
/// @brief 从 JSON 读取背景配置。
void from_json(const nlohmann::json& json, BackgroundConfig& config);

}  // namespace MMM::Config
