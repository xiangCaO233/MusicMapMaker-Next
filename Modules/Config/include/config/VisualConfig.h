#pragma once
#include "config/BeatLinePalette.h"
#include <cstdint>
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

/// @brief 频谱图生成精细度。
enum class SpectrumDetailLevel {
    Performance,
    Balanced,
    Fine,
    Ultra,
    Extreme,
    Experimental,
};

NLOHMANN_JSON_SERIALIZE_ENUM(
    SpectrumDetailLevel,
    {
        { SpectrumDetailLevel::Performance, "Performance" },
        { SpectrumDetailLevel::Balanced, "Balanced" },
        { SpectrumDetailLevel::Fine, "Fine" },
        { SpectrumDetailLevel::Ultra, "Ultra" },
        { SpectrumDetailLevel::Extreme, "Extreme" },
        { SpectrumDetailLevel::Experimental, "Experimental" },
    })

/// @brief 频谱图精细度对应的生成参数。
struct SpectrumDetailProfile {
    /// @brief 时间分辨率，单位为段/秒。
    double segmentsPerSecond{ 100.0 };

    /// @brief 频率方向分箱数。
    int frequencyBins{ 128 };
};

/// @brief 获取频谱图精细度对应的生成参数。
/// @param level 频谱图精细度。
/// @return 频谱图生成参数。
inline SpectrumDetailProfile spectrumDetailProfile(SpectrumDetailLevel level)
{
    switch ( level ) {
    case SpectrumDetailLevel::Performance: return { 40.0, 64 };
    case SpectrumDetailLevel::Balanced: return { 64.0, 96 };
    case SpectrumDetailLevel::Fine: return { 96.0, 128 };
    case SpectrumDetailLevel::Ultra: return { 160.0, 192 };
    case SpectrumDetailLevel::Extreme: return { 240.0, 256 };
    case SpectrumDetailLevel::Experimental: return { 360.0, 384 };
    }
    return { 64.0, 96 };
}

/// @brief 估算指定频谱图精细度每分钟需要的纹理显存。
/// @param level 频谱图精细度。
/// @param channelCount 频谱图纹理通道数量。
/// @param bytesPerTexel 每个纹理像素占用的字节数。
/// @return 每分钟纹理显存字节数，不含驱动对齐和描述符开销。
inline std::uint64_t estimateSpectrumTextureBytesPerMinute(
    SpectrumDetailLevel level, std::uint32_t channelCount,
    std::uint32_t bytesPerTexel = 1)
{
    const auto profile = spectrumDetailProfile(level);
    return static_cast<std::uint64_t>(
        profile.segmentsPerSecond * 60.0 *
        static_cast<double>(profile.frequencyBins) *
        static_cast<double>(bytesPerTexel) * static_cast<double>(channelCount));
}

struct TrackLayout {
    /// @brief 左侧分隔比例位置
    float left{ .2f };
    /// @brief 顶部分隔比例位置
    float top{ .05f };
    /// @brief 右侧分隔比例位置
    float right{ .8f };
    /// @brief 底部分隔比例位置
    float bottom{ .95f };
};

inline void to_json(nlohmann::json& j, const TrackLayout& c)
{
    j = nlohmann::json{ { "left", c.left },
                        { "top", c.top },
                        { "right", c.right },
                        { "bottom", c.bottom } };
}

inline void from_json(const nlohmann::json& j, TrackLayout& c)
{
    c.left   = j.value("left", .2f);
    c.top    = j.value("top", .05f);
    c.right  = j.value("right", .8f);
    c.bottom = j.value("bottom", .95f);
}

struct BackgroundConfig {
    /// @brief 填充模式
    BackgroundFillMode fillMode{ BackgroundFillMode::AspectFill };
    /// @brief 背景暗化比例-0.0代表完全透明，1.0代表完全显示
    float darken_ratio{ .7f };
    /// @brief 背景不透明度-0.0代表完全透明
    float opaque_ratio{ 1.f };
};

inline void to_json(nlohmann::json& j, const BackgroundConfig& c)
{
    j = nlohmann::json{ { "fillMode", c.fillMode },
                        { "darken_ratio", c.darken_ratio },
                        { "opaque_ratio", c.opaque_ratio } };
}

inline void from_json(const nlohmann::json& j, BackgroundConfig& c)
{
    c.fillMode     = j.value("fillMode", BackgroundFillMode::AspectFill);
    c.darken_ratio = j.value("darken_ratio", .7f);
    c.opaque_ratio = j.value("opaque_ratio", 1.f);
}

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
    };
    AreaMargin margin;
    /// @brief 是否绘制分拍线
    bool drawBeatLines{ true };
    /// @brief 是否绘制 Timing 线
    bool drawTimingLines{ true };
};

inline void to_json(nlohmann::json& j, const PreviewAreaConfig::AreaMargin& c)
{
    j = nlohmann::json{ { "left", c.left },
                        { "top", c.top },
                        { "right", c.right },
                        { "bottom", c.bottom } };
}

inline void from_json(const nlohmann::json& j, PreviewAreaConfig::AreaMargin& c)
{
    c.left   = j.value("left", 4.f);
    c.top    = j.value("top", 4.f);
    c.right  = j.value("right", 4.f);
    c.bottom = j.value("bottom", 4.f);
}

inline void to_json(nlohmann::json& j, const PreviewAreaConfig& c)
{
    j = nlohmann::json{ { "areaRatio", c.areaRatio },
                        { "edgeScrollSensitivity", c.edgeScrollSensitivity },
                        { "margin", c.margin },
                        { "drawBeatLines", c.drawBeatLines },
                        { "drawTimingLines", c.drawTimingLines } };
}

inline void from_json(const nlohmann::json& j, PreviewAreaConfig& c)
{
    c.areaRatio             = j.value("areaRatio", 5.f);
    c.edgeScrollSensitivity = j.value("edgeScrollSensitivity", 1.0f);
    c.margin          = j.value("margin", PreviewAreaConfig::AreaMargin());
    c.drawBeatLines   = j.value("drawBeatLines", true);
    c.drawTimingLines = j.value("drawTimingLines", true);
}

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
    float noteScaleX{ 1.2f };
    /// @brief 音符 Y 轴缩放
    float noteScaleY{ 1.2f };
    /// @brief 音符填充模式
    BackgroundFillMode noteFillMode{ BackgroundFillMode::Stretch };
    /// @brief 视觉偏移
    float visualOffset{ 0.0f };
    /// @brief 波形图专用采样偏移，仅影响波形采样内容的显示位置
    float waveformVisualOffset{ 0.0f };
    /// @brief 频谱图专用采样偏移，仅影响频谱采样内容的显示位置
    float spectrumVisualOffset{ 0.0f };
    /// @brief 固定的硬件/多平台视觉偏置 (只读，加算到任何使用 visualOffset
    /// 的地方)
    static constexpr float staticVisualOffset{ -0.035f };
    /// @brief 获取应用偏置后的最终视觉偏移量
    float getEffectiveVisualOffset() const
    {
        return visualOffset + staticVisualOffset;
    }
    /// @brief 获取波形图采样内容专用偏移量
    float getWaveformEffectiveVisualOffset() const
    {
        return waveformVisualOffset;
    }
    /// @brief 获取频谱图采样内容专用偏移量
    float getSpectrumEffectiveVisualOffset() const
    {
        return spectrumVisualOffset;
    }
    /// @brief 时间轴缩放
    float timelineZoom{ 1.0f };
    /// @brief 滚动/跳转后的视觉平滑动画时间，单位秒；0 表示关闭动画
    float scrollAnimationDuration{ 0.12f };
    /// @brief 是否启用线性滚动映射 (通常用于预览)
    bool enableLinearScrollMapping{ false };
    /// @brief 鼠标吸附阈值
    float snapThreshold{ 16.0f };
    /// @brief 分拍线不透明度
    float beatLineAlpha{ 0.75f };
    /// @brief 是否以当前调色方案覆盖皮肤分拍线颜色；仅保留在运行时。
    bool overrideBeatLineColors{ false };
    /// @brief 当前调色方案的分拍线覆盖颜色；仅保留在运行时。
    BeatLineColorPalette beatLineColors{};
    /// @brief 是否绘制第一个 BPM 红线前的分拍线
    bool drawBeatLinesBeforeFirstTiming{ true };
    /// @brief 是否全局绘制分拍线 (主画布与预览区同步)
    bool drawBeatLines{ true };
    /// @brief 全局频谱图生成精细度。
    SpectrumDetailLevel spectrumDetailLevel{ SpectrumDetailLevel::Balanced };
    /// @brief 是否启用打击特效动画
    bool enableHitEffects{ true };
    /// @brief 是否绘制音符悬浮拾取包围盒，主要用于调试交互命中区域。
    bool debugDrawHitboxes{ false };
};

inline void to_json(nlohmann::json& j, const VisualConfig& c)
{
    j = nlohmann::json{
        { "trackLayout", c.trackLayout },
        { "background", c.background },
        { "previewConfig", c.previewConfig },
        { "trackBoxLineWidth", c.trackBoxLineWidth },
        { "judgeline_pos", c.judgeline_pos },
        { "noteScaleX", c.noteScaleX },
        { "noteScaleY", c.noteScaleY },
        { "noteFillMode", c.noteFillMode },
        { "visualOffset", c.visualOffset },
        { "waveformVisualOffset", c.waveformVisualOffset },
        { "spectrumVisualOffset", c.spectrumVisualOffset },
        { "timelineZoom", c.timelineZoom },
        { "scrollAnimationDuration", c.scrollAnimationDuration },
        { "enableLinearScrollMapping", c.enableLinearScrollMapping },
        { "snapThreshold", c.snapThreshold },
        { "beatLineAlpha", c.beatLineAlpha },
        { "drawBeatLinesBeforeFirstTiming", c.drawBeatLinesBeforeFirstTiming },
        { "drawBeatLines", c.drawBeatLines },
        { "spectrumDetailLevel", c.spectrumDetailLevel },
        { "enableHitEffects", c.enableHitEffects },
        { "debugDrawHitboxes", c.debugDrawHitboxes }
    };
}

inline void from_json(const nlohmann::json& j, VisualConfig& c)
{
    c.trackLayout       = j.value("trackLayout", TrackLayout());
    c.background        = j.value("background", BackgroundConfig());
    c.previewConfig     = j.value("previewConfig", PreviewAreaConfig());
    c.trackBoxLineWidth = j.value("trackBoxLineWidth", 1.5f);
    c.judgeline_pos     = j.value("judgeline_pos", 0.85f);
    c.noteScaleX        = j.value("noteScaleX", VisualConfig{}.noteScaleX);
    c.noteScaleY        = j.value("noteScaleY", VisualConfig{}.noteScaleY);
    c.noteFillMode      = j.value("noteFillMode", BackgroundFillMode::Stretch);
    c.visualOffset      = j.value("visualOffset", 0.0f);
    c.waveformVisualOffset      = j.value("waveformVisualOffset", 0.0f);
    c.spectrumVisualOffset      = j.value("spectrumVisualOffset", 0.0f);
    c.timelineZoom              = j.value("timelineZoom", 1.0f);
    c.scrollAnimationDuration   = j.value("scrollAnimationDuration", 0.12f);
    c.enableLinearScrollMapping = j.value("enableLinearScrollMapping", false);
    c.snapThreshold             = j.value("snapThreshold", 16.0f);
    c.beatLineAlpha             = j.value("beatLineAlpha", 0.75f);
    c.overrideBeatLineColors    = false;
    c.beatLineColors            = {};
    c.drawBeatLines             = j.value("drawBeatLines", true);
    c.drawBeatLinesBeforeFirstTiming =
        j.value("drawBeatLinesBeforeFirstTiming", true);
    c.spectrumDetailLevel =
        j.value("spectrumDetailLevel", SpectrumDetailLevel::Balanced);
    c.enableHitEffects  = j.value("enableHitEffects", true);
    c.debugDrawHitboxes = j.value("debugDrawHitboxes", false);
}

}  // namespace MMM::Config
