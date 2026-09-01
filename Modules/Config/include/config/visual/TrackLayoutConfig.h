#pragma once

#include <nlohmann/json_fwd.hpp>
#include <optional>

namespace MMM::Config
{

/// @brief 主画布辅助区域的可选归一化横向布局。
struct HorizontalRegionLayout {
    /// @brief 自定义左边界比例；空值表示沿用兼容布局。
    std::optional<float> left;
    /// @brief 自定义宽度比例；轨道区表示单轨宽度，空值表示沿用兼容布局。
    std::optional<float> width;
};

/// @brief 将辅助区域横向布局序列化为 JSON。
void to_json(nlohmann::json& json, const HorizontalRegionLayout& layout);
/// @brief 从 JSON 读取辅助区域横向布局。
void from_json(const nlohmann::json& json, HorizontalRegionLayout& layout);

/// @brief 主画布轨道矩形及辅助区域的布局配置。
struct TrackLayout {
    /// @brief 左侧分隔比例位置。
    float left{ 0.2f };
    /// @brief 顶部分隔比例位置。
    float top{ 0.05f };
    /// @brief 右侧分隔比例位置。
    float right{ 0.8f };
    /// @brief 底部分隔比例位置。
    float bottom{ 0.95f };
    /// @brief 草稿轨道区独立横向布局；宽度表示单条草稿轨宽度。
    HorizontalRegionLayout draftLanes;
    /// @brief 批注区独立横向布局；宽度表示整个批注区宽度。
    HorizontalRegionLayout annotation;
    /// @brief BGM 轨道区独立横向布局；宽度表示单条 BGM 轨宽度。
    HorizontalRegionLayout bgmLanes;
};

/// @brief 将轨道布局序列化为 JSON。
void to_json(nlohmann::json& json, const TrackLayout& layout);
/// @brief 从 JSON 读取轨道布局。
void from_json(const nlohmann::json& json, TrackLayout& layout);

}  // namespace MMM::Config
