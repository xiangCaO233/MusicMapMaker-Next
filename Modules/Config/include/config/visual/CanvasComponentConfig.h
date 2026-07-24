#pragma once

#include <array>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>

namespace MMM::Config
{

/// @brief 可绘制到主画布最终覆盖层的组件类型。
enum class CanvasComponentType : std::uint8_t {
    JudgmentLineTime,
    BeatNumber,
    Count,
};

/// @brief 全部画布组件类型的稳定遍历顺序。
inline constexpr std::array<CanvasComponentType, 2> CANVAS_COMPONENT_TYPES{
    CanvasComponentType::JudgmentLineTime,
    CanvasComponentType::BeatNumber,
};

/// @brief 单个画布组件的显隐、归一化锚点、字号与颜色配置。
struct CanvasComponentPlacement {
    /// @brief 是否在每个主画布上绘制该组件。
    bool visible{ false };
    /// @brief 组件中心相对画布宽度的横向比例。
    float anchorX{ 0.5f };
    /// @brief 组件中心相对画布高度的纵向比例。
    float anchorY{ 0.12f };
    /// @brief 字号相对画布高度的比例。
    float fontSizeRatio{ 0.035f };
    /// @brief 组件内容 RGBA 颜色。
    std::array<float, 4> color{ 1.0f, 1.0f, 1.0f, 1.0f };
};

/// @brief 拍号组件的默认拍内布局。
inline constexpr CanvasComponentPlacement DEFAULT_BEAT_NUMBER_PLACEMENT{
    false, 0.08f, 0.5f, 0.18f, { 1.0f, 140.0f / 255.0f, 0.0f, 1.0f },
};

/// @brief 主画布可选组件的布局集合。
struct CanvasComponentLayoutConfig {
    /// @brief 当前判定线时间组件布局。
    CanvasComponentPlacement judgmentLineTime;
    /// @brief 逐拍绘制的拍号组件拍内布局。
    CanvasComponentPlacement beatNumber{ DEFAULT_BEAT_NUMBER_PLACEMENT };

    /// @brief 按组件类型取得可写布局。
    /// @param type 组件类型。
    /// @return 对应组件的布局引用。
    CanvasComponentPlacement& placement(CanvasComponentType type)
    {
        switch ( type ) {
        case CanvasComponentType::JudgmentLineTime: return judgmentLineTime;
        case CanvasComponentType::BeatNumber: return beatNumber;
        case CanvasComponentType::Count: break;
        }
        return judgmentLineTime;
    }

    /// @brief 按组件类型取得只读布局。
    /// @param type 组件类型。
    /// @return 对应组件的布局引用。
    const CanvasComponentPlacement& placement(CanvasComponentType type) const
    {
        switch ( type ) {
        case CanvasComponentType::JudgmentLineTime: return judgmentLineTime;
        case CanvasComponentType::BeatNumber: return beatNumber;
        case CanvasComponentType::Count: break;
        }
        return judgmentLineTime;
    }
};

/// @brief 将单个画布组件布局序列化为 JSON。
void to_json(nlohmann::json& json, const CanvasComponentPlacement& placement);
/// @brief 从 JSON 读取单个画布组件布局。
void from_json(const nlohmann::json& json, CanvasComponentPlacement& placement);
/// @brief 将画布组件布局集合序列化为 JSON。
void to_json(nlohmann::json& json, const CanvasComponentLayoutConfig& config);
/// @brief 从 JSON 读取画布组件布局集合。
void from_json(const nlohmann::json& json, CanvasComponentLayoutConfig& config);

}  // namespace MMM::Config
