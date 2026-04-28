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
    /// @brief 轨道布局比例
    TrackLayout trackLayout;

    /// @brief 背景渲染配置
    BackgroundConfig background;

    /// @brief 预览区视口配置
    PreviewAreaConfig previewConfig;

    /// @brief 轨道布局包围框线宽(px)
    float trackBoxLineWidth{ 2.0f };

    /// @brief 判定线y方向比例位置 (0.0~1.0)
    float judgeline_pos{ .85f };

    /// @brief 判定线线宽(px)
    float judgelineWidth{ 4.0f };

    /// @brief 物件在X/Y方向的渲染缩放倍率
    float noteScaleX{ 1.25f };
    float noteScaleY{ 1.25f };

    /// @brief 物件（如长条）内部纹理的填充模式
    BackgroundFillMode noteFillMode{ BackgroundFillMode::Stretch };

    /// @brief 视觉偏移量 (秒)，用于补偿渲染与音频的延迟
    float visualOffset{ -0.040f };

    /// @brief 时间线缩放倍率 (1.0 代表原始比例)
    float timelineZoom{ 1.0f };

    /// @brief 是否启用线性流速映射 (忽略变速事件，以匀速显示)
    bool enableLinearScrollMapping{ false };

    /// @brief 磁吸阈值 (px)，鼠标距离拍线在此范围内触发磁吸提示
    float snapThreshold{ 32.0f };

    /// @brief 分拍线透明度 (0.0~1.0)
    float beatLineAlpha{ 1.0f };

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(VisualConfig, trackLayout, background,
                                   previewConfig, trackBoxLineWidth,
                                   judgeline_pos, judgelineWidth, noteScaleX,
                                   noteScaleY, noteFillMode, visualOffset,
                                   timelineZoom, enableLinearScrollMapping,
                                   snapThreshold, beatLineAlpha)
};

}  // namespace MMM::Config
