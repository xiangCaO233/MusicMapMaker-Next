#pragma once
#include <nlohmann/json.hpp>

namespace MMM::Config
{

enum class BackgroundFillMode {
    Stretch,     // 拉伸填充 (不保持比例)
    AspectFit,   // 保持比例缩放以适应 (显示全貌，可能有黑边)
    AspectFill,  // 保持比例缩放并填充 (裁切边缘，无黑边)
    Center       // 原始大小居中
};

NLOHMANN_JSON_SERIALIZE_ENUM(BackgroundFillMode,
                             {
                                 { BackgroundFillMode::Stretch, "Stretch" },
                                 { BackgroundFillMode::AspectFit, "AspectFit" },
                                 { BackgroundFillMode::AspectFill,
                                   "AspectFill" },
                                 { BackgroundFillMode::Center, "Center" },
                             })

struct TrackLayout {
    /// @brief 左侧分隔比例位置
    float left{ .2f };
    /// @brief 顶部分隔比例位置
    float top{ .05f };
    /// @brief 右侧分隔比例位置
    float right{ .8f };
    /// @brief 底部分隔比例位置
    float bottom{ .95f };

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(TrackLayout, left, top, right, bottom)
};

struct BackgroundConfig {
    /// @brief 填充模式
    BackgroundFillMode fillMode{ BackgroundFillMode::AspectFill };
    /// @brief 背景暗化比例-0.0代表完全透明，1.0代表完全显示
    float darken_ratio{ .7f };
    /// @brief 背景不透明度-0.0代表完全透明
    float opaque_ratio{ 1.f };

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(BackgroundConfig, fillMode, darken_ratio,
                                   opaque_ratio)
};

struct PreviewAreaConfig {
    /// @brief 预览区视口范围相对主画布的倍率
    float areaRatio{ 5.f };

    /// @brief 边缘自动滚动灵敏度倍率
    float edgeScrollSensitivity{ 1.0f };

    /// @brief 预览区留白(px)
    struct AreaMargin {
        float left{ 4.f };
        float top{ 4.f };
        float right{ 4.f };
        float bottom{ 4.f };

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(AreaMargin, left, top, right, bottom)
    };
    AreaMargin margin;
    /// @brief 是否绘制分拍线
    bool drawBeatLines{ true };
    /// @brief 是否绘制 Timing 线
    bool drawTimingLines{ true };

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(PreviewAreaConfig, areaRatio,
                                   edgeScrollSensitivity, margin, drawBeatLines,
                                   drawTimingLines)
};

/// @brief 视觉与渲染相关的整体配置
struct VisualConfig {
    TrackLayout       trackLayout;
    BackgroundConfig  background;
    PreviewAreaConfig previewConfig;

    /// @brief 轨道线线宽
    float trackBoxLineWidth{ 1.5f };
    /// @brief 判定线位置 (0.0 - 1.0)
    float judgeline_pos{ .85f };
    /// @brief 音符 X 轴缩放
    float noteScaleX{ 1.0f };
    /// @brief 音符 Y 轴缩放
    float noteScaleY{ 1.0f };
    /// @brief 音符填充模式
    BackgroundFillMode noteFillMode{ BackgroundFillMode::Stretch };
    /// @brief 视觉偏移
    float visualOffset{ 0.0f };
    /// @brief 时间轴缩放
    float timelineZoom{ 1.0f };
    /// @brief 是否启用线性滚动映射 (通常用于预览)
    bool enableLinearScrollMapping{ false };
    /// @brief 鼠标吸附阈值
    float snapThreshold{ 16.0f };
    /// @brief 分拍线不透明度
    float beatLineAlpha{ 1.0f };
    /// @brief 是否全局绘制分拍线 (主画布与预览区同步)
    bool drawBeatLines{ true };
};

inline void to_json(nlohmann::json& j, const VisualConfig& c)
{
    j = nlohmann::json{ { "trackLayout", c.trackLayout },
                        { "background", c.background },
                        { "previewConfig", c.previewConfig },
                        { "trackBoxLineWidth", c.trackBoxLineWidth },
                        { "judgeline_pos", c.judgeline_pos },
                        { "noteScaleX", c.noteScaleX },
                        { "noteScaleY", c.noteScaleY },
                        { "noteFillMode", c.noteFillMode },
                        { "visualOffset", c.visualOffset },
                        { "timelineZoom", c.timelineZoom },
                        { "enableLinearScrollMapping",
                          c.enableLinearScrollMapping },
                        { "snapThreshold", c.snapThreshold },
                        { "beatLineAlpha", c.beatLineAlpha },
                        { "drawBeatLines", c.drawBeatLines } };
}

inline void from_json(const nlohmann::json& j, VisualConfig& c)
{
    c.trackLayout       = j.value("trackLayout", TrackLayout());
    c.background        = j.value("background", BackgroundConfig());
    c.previewConfig     = j.value("previewConfig", PreviewAreaConfig());
    c.trackBoxLineWidth = j.value("trackBoxLineWidth", 1.5f);
    c.judgeline_pos     = j.value("judgeline_pos", 0.85f);
    c.noteScaleX        = j.value("noteScaleX", 1.0f);
    c.noteScaleY        = j.value("noteScaleY", 1.0f);
    c.noteFillMode      = j.value("noteFillMode", BackgroundFillMode::Stretch);
    c.visualOffset      = j.value("visualOffset", 0.0f);
    c.timelineZoom      = j.value("timelineZoom", 1.0f);
    c.enableLinearScrollMapping = j.value("enableLinearScrollMapping", false);
    c.snapThreshold             = j.value("snapThreshold", 16.0f);
    c.beatLineAlpha             = j.value("beatLineAlpha", 1.0f);
    c.drawBeatLines             = j.value("drawBeatLines", true);
}

}  // namespace MMM::Config
