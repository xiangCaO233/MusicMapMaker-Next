#pragma once

#include "config/BeatLinePalette.h"
#include "config/visual/BackgroundConfig.h"
#include "config/visual/CanvasComponentConfig.h"
#include "config/visual/PreviewAreaConfig.h"
#include "config/visual/SpectrumConfig.h"
#include "config/visual/TrackLayoutConfig.h"
#include <nlohmann/json_fwd.hpp>

namespace MMM::Config
{

/// @brief 主画布分拍线显示模式。
enum class BeatLineDisplayMode {
    Always,      ///< 始终显示分拍线。
    NearCursor,  ///< 仅在光标附近显示并向外渐隐。
    Hidden       ///< 隐藏分拍线。
};

/// @brief 将分拍线显示模式序列化为稳定文本。
void to_json(nlohmann::json& json, const BeatLineDisplayMode& mode);
/// @brief 从稳定文本读取分拍线显示模式。
void from_json(const nlohmann::json& json, BeatLineDisplayMode& mode);

/// @brief 视觉与渲染相关的整体配置。
struct VisualConfig {
    TrackLayout                 trackLayout;
    CanvasComponentLayoutConfig canvasComponents;
    BackgroundConfig            background;
    PreviewAreaConfig           previewConfig;

    /// @brief 轨道线线宽。
    float trackBoxLineWidth{ 1.5f };
    /// @brief 判定线位置。
    float judgeline_pos{ 0.85f };
    /// @brief 音符 X 轴缩放。
    float noteScaleX{ 1.2f };
    /// @brief 音符 Y 轴缩放。
    float noteScaleY{ 1.2f };
    /// @brief 是否在玩家物件上方显示绑定音效资源标签。
    bool showBoundSampleLabels{ false };
    /// @brief 音符填充模式。
    BackgroundFillMode noteFillMode{ BackgroundFillMode::Stretch };
    /// @brief 视觉偏移。
    float visualOffset{ 0.0f };
    /// @brief 波形图专用采样偏移，仅影响波形采样内容的显示位置。
    float waveformVisualOffset{ 0.0f };
    /// @brief 频谱图专用采样偏移，仅影响频谱采样内容的显示位置。
    float spectrumVisualOffset{ 0.0f };
    /// @brief 固定的硬件与多平台视觉偏置。
    static constexpr float staticVisualOffset{ -0.035f };

    /// @brief 获取应用偏置后的最终视觉偏移量。
    /// @return 最终视觉偏移。
    float getEffectiveVisualOffset() const
    {
        return visualOffset + staticVisualOffset;
    }

    /// @brief 获取波形图采样内容专用偏移量。
    /// @return 波形图采样偏移。
    float getWaveformEffectiveVisualOffset() const
    {
        return waveformVisualOffset;
    }

    /// @brief 获取频谱图采样内容专用偏移量。
    /// @return 频谱图采样偏移。
    float getSpectrumEffectiveVisualOffset() const
    {
        return spectrumVisualOffset;
    }

    /// @brief 时间轴缩放。
    float timelineZoom{ 1.0f };
    /// @brief 滚动或跳转后的视觉平滑动画时间，单位秒。
    float scrollAnimationDuration{ 0.12f };
    /// @brief 是否启用线性滚动映射。
    bool enableLinearScrollMapping{ false };
    /// @brief 鼠标吸附阈值。
    float snapThreshold{ 16.0f };
    /// @brief 分拍线不透明度。
    float beatLineAlpha{ 0.75f };
    /// @brief 主画布分拍线显示模式。
    BeatLineDisplayMode beatLineDisplayMode{ BeatLineDisplayMode::Always };
    /// @brief 自动模式下完全显示区域占画布垂直范围的比例。
    float beatLineCursorVisibleRatio{ 0.16f };
    /// @brief 自动模式下两侧渐隐扩散区域合计占画布垂直范围的比例。
    float beatLineCursorFadeRatio{ 0.20f };
    /// @brief 是否以当前调色方案覆盖皮肤分拍线颜色；仅保留在运行时。
    bool overrideBeatLineColors{ false };
    /// @brief 当前调色方案的分拍线覆盖颜色；仅保留在运行时。
    BeatLineColorPalette beatLineColors{};
    /// @brief 是否绘制第一个 BPM 红线前的分拍线。
    bool drawBeatLinesBeforeFirstTiming{ true };
    /// @brief 全局频谱图生成精细度。
    SpectrumDetailLevel spectrumDetailLevel{ SpectrumDetailLevel::Balanced };
    /// @brief 是否启用打击特效动画。
    bool enableHitEffects{ true };
    /// @brief 是否绘制音符悬浮拾取包围盒。
    bool debugDrawHitboxes{ false };
};

/// @brief 将完整视觉配置序列化为 JSON。
void to_json(nlohmann::json& json, const VisualConfig& config);
/// @brief 从 JSON 读取完整视觉配置。
void from_json(const nlohmann::json& json, VisualConfig& config);

}  // namespace MMM::Config
