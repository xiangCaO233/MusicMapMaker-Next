#pragma once

#include <nlohmann/json_fwd.hpp>

namespace MMM::Config
{

/// @brief 主画布轨道矩形的归一化边界。
struct TrackLayout {
    /// @brief 左侧分隔比例位置。
    float left{ 0.2f };
    /// @brief 顶部分隔比例位置。
    float top{ 0.05f };
    /// @brief 右侧分隔比例位置。
    float right{ 0.8f };
    /// @brief 底部分隔比例位置。
    float bottom{ 0.95f };
};

/// @brief 将轨道布局序列化为 JSON。
void to_json(nlohmann::json& json, const TrackLayout& layout);
/// @brief 从 JSON 读取轨道布局。
void from_json(const nlohmann::json& json, TrackLayout& layout);

}  // namespace MMM::Config
