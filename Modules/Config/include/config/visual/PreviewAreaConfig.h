#pragma once

#include <nlohmann/json_fwd.hpp>

namespace MMM::Config
{

/// @brief 预览画布的显示范围与内容开关。
struct PreviewAreaConfig {
    /// @brief 预览区视口范围相对主画布的倍率。
    float areaRatio{ 5.0f };

    /// @brief 边缘自动滚动灵敏度倍率。
    float edgeScrollSensitivity{ 1.0f };

    /// @brief 预览区像素留白。
    struct AreaMargin {
        float left{ 4.0f };
        float top{ 4.0f };
        float right{ 4.0f };
        float bottom{ 4.0f };
    };

    AreaMargin margin;
    /// @brief 是否绘制分拍线。
    bool drawBeatLines{ false };
    /// @brief 是否绘制 Timing 线。
    bool drawTimingLines{ true };
};

/// @brief 将预览区留白序列化为 JSON。
void to_json(nlohmann::json& json, const PreviewAreaConfig::AreaMargin& margin);
/// @brief 从 JSON 读取预览区留白。
void from_json(const nlohmann::json&          json,
               PreviewAreaConfig::AreaMargin& margin);
/// @brief 将预览区配置序列化为 JSON。
void to_json(nlohmann::json& json, const PreviewAreaConfig& config);
/// @brief 从 JSON 读取预览区配置。
void from_json(const nlohmann::json& json, PreviewAreaConfig& config);

}  // namespace MMM::Config
