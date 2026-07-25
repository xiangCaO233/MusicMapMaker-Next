#pragma once

#include <array>
#include <cstdint>
#include <nlohmann/json_fwd.hpp>
#include <vector>

namespace MMM::Config
{

/// @brief 可绘制到主画布最终覆盖层的组件类型。
enum class CanvasComponentType : std::uint8_t {
    JudgmentLineTime,
    BeatNumber,
    BeatLineTime,
    Kps,
    Count,
};

/// @brief 全部画布组件类型的稳定遍历顺序。
inline constexpr std::array<CanvasComponentType, 4> CANVAS_COMPONENT_TYPES{
    CanvasComponentType::JudgmentLineTime,
    CanvasComponentType::BeatNumber,
    CanvasComponentType::BeatLineTime,
    CanvasComponentType::Kps,
};

/// @brief KPS 总计文字使用的稳定实例序号。
inline constexpr std::int64_t KPS_TOTAL_INSTANCE_INDEX = -1;

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

/// @brief 当前判定线时间组件的默认画布布局。
inline constexpr CanvasComponentPlacement
    DEFAULT_JUDGMENT_LINE_TIME_PLACEMENT{};

/// @brief 拍号组件的默认拍内布局。
inline constexpr CanvasComponentPlacement DEFAULT_BEAT_NUMBER_PLACEMENT{
    false, 0.08f, 0.5f, 0.18f, { 1.0f, 140.0f / 255.0f, 0.0f, 1.0f },
};

/// @brief 分拍线时间组件的默认分拍内布局。
inline constexpr CanvasComponentPlacement DEFAULT_BEAT_LINE_TIME_PLACEMENT{
    false, 0.22f, 0.5f, 0.18f, { 1.0f, 1.0f, 1.0f, 1.0f },
};

/// @brief KPS 总计组件的默认画布布局。
inline constexpr CanvasComponentPlacement DEFAULT_KPS_TOTAL_PLACEMENT{
    false, 0.5f, 0.08f, 0.035f, { 1.0f, 1.0f, 1.0f, 1.0f },
};

/// @brief 单条轨道 KPS 组件的布局覆盖。
struct CanvasKpsTrackPlacement {
    /// @brief 从零开始的轨道序号。
    std::int32_t trackIndex{ 0 };
    /// @brief 该轨道 KPS 文字的独立布局。
    CanvasComponentPlacement placement;
};

/// @brief 主画布可选组件的布局集合。
struct CanvasComponentLayoutConfig {
    /// @brief 当前判定线时间组件布局。
    CanvasComponentPlacement judgmentLineTime{
        DEFAULT_JUDGMENT_LINE_TIME_PLACEMENT
    };
    /// @brief 逐拍绘制的拍号组件拍内布局。
    CanvasComponentPlacement beatNumber{ DEFAULT_BEAT_NUMBER_PLACEMENT };
    /// @brief 逐分拍绘制的分拍线时间组件布局。
    CanvasComponentPlacement beatLineTime{ DEFAULT_BEAT_LINE_TIME_PLACEMENT };
    /// @brief KPS 总计布局，同时提供整组 KPS 的显隐与颜色。
    CanvasComponentPlacement kps{ DEFAULT_KPS_TOTAL_PLACEMENT };
    /// @brief 用户调整过的逐轨 KPS 独立布局。
    std::vector<CanvasKpsTrackPlacement> kpsTracks;
    /// @brief 是否在缩放任意逐轨 KPS 时同步全部逐轨字号。
    bool syncKpsTrackSizes{ false };
    /// @brief 是否在移动任意逐轨 KPS 时同步全部逐轨相对位置。
    bool syncKpsTrackRelativePositions{ false };
    /// @brief 最近一次批量同步的逐轨 KPS 字号；零表示使用轨道数自适应字号。
    float kpsTrackFontSizeRatio{ 0.0f };

    /// @brief 按组件类型取得可写布局。
    /// @param type 组件类型。
    /// @return 对应组件的布局引用。
    CanvasComponentPlacement& placement(CanvasComponentType type)
    {
        switch ( type ) {
        case CanvasComponentType::JudgmentLineTime: return judgmentLineTime;
        case CanvasComponentType::BeatNumber: return beatNumber;
        case CanvasComponentType::BeatLineTime: return beatLineTime;
        case CanvasComponentType::Kps: return kps;
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
        case CanvasComponentType::BeatLineTime: return beatLineTime;
        case CanvasComponentType::Kps: return kps;
        case CanvasComponentType::Count: break;
        }
        return judgmentLineTime;
    }

    /// @brief 取得指定组件实例当前实际使用的布局。
    /// @param type 组件类型。
    /// @param instanceIndex 组件实例序号；KPS 总计使用
    /// `KPS_TOTAL_INSTANCE_INDEX`。
    /// @param trackCount 当前谱面轨道数量。
    /// @param trackLeft 轨道区域左边界占画布宽度的比例。
    /// @param trackRight 轨道区域右边界占画布宽度的比例。
    /// @return 已保存覆盖或按当前轨道数计算的默认布局。
    /// @warning 渲染热路径：每个 KPS 实例调用；只允许有序覆盖查找与值拷贝。
    [[nodiscard]] CanvasComponentPlacement resolvedPlacement(
        CanvasComponentType type, std::int64_t instanceIndex,
        std::int32_t trackCount, float trackLeft, float trackRight) const;

    /// @brief 取得指定组件实例的可写布局，必要时建立逐轨 KPS 覆盖。
    /// @param type 组件类型。
    /// @param instanceIndex 组件实例序号；KPS 总计使用
    /// `KPS_TOTAL_INSTANCE_INDEX`。
    /// @param trackCount 当前谱面轨道数量。
    /// @param trackLeft 轨道区域左边界占画布宽度的比例。
    /// @param trackRight 轨道区域右边界占画布宽度的比例。
    /// @return 对应实例的可写布局引用。
    CanvasComponentPlacement& editablePlacement(CanvasComponentType type,
                                                std::int64_t instanceIndex,
                                                std::int32_t trackCount,
                                                float        trackLeft,
                                                float        trackRight);

    /// @brief 将全部逐轨 KPS 的字号同步为同一比例。
    /// @param fontSizeRatio 字号相对画布高度的比例。
    /// @warning 布局拖动路径：逐轨 KPS 同步缩放时调用；只遍历已保存的覆盖项。
    void synchronizeKpsTrackFontSize(float fontSizeRatio);

    /// @brief 将指定组件的位置和尺寸恢复为默认值。
    /// @param type 需要复位的组件类型。
    /// @warning UI 低频路径：用户点击复位按钮时调用；KPS 会清除逐轨布局覆盖。
    void resetPlacementToDefault(CanvasComponentType type);
};

/// @brief 将单个画布组件布局序列化为 JSON。
void to_json(nlohmann::json& json, const CanvasComponentPlacement& placement);
/// @brief 从 JSON 读取单个画布组件布局。
void from_json(const nlohmann::json& json, CanvasComponentPlacement& placement);
/// @brief 将逐轨 KPS 布局覆盖序列化为 JSON。
void to_json(nlohmann::json& json, const CanvasKpsTrackPlacement& placement);
/// @brief 从 JSON 读取逐轨 KPS 布局覆盖。
void from_json(const nlohmann::json& json, CanvasKpsTrackPlacement& placement);
/// @brief 将画布组件布局集合序列化为 JSON。
void to_json(nlohmann::json& json, const CanvasComponentLayoutConfig& config);
/// @brief 从 JSON 读取画布组件布局集合。
void from_json(const nlohmann::json& json, CanvasComponentLayoutConfig& config);

}  // namespace MMM::Config
