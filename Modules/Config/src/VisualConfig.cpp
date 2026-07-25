#include "config/VisualConfig.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>

namespace MMM::Config
{

namespace
{
/// @brief 旧版拍号与分拍线时间相对局部区间高度的默认字号比例。
constexpr float LEGACY_REPEATED_TEXT_FONT_SIZE_RATIO = 0.18f;
}  // namespace

void to_json(nlohmann::json& j, const BeatLineDisplayMode& mode)
{
    switch ( mode ) {
    case BeatLineDisplayMode::Always: j = "Always"; break;
    case BeatLineDisplayMode::NearCursor: j = "NearCursor"; break;
    case BeatLineDisplayMode::Hidden: j = "Hidden"; break;
    }
}

void from_json(const nlohmann::json& j, BeatLineDisplayMode& mode)
{
    mode = BeatLineDisplayMode::Always;
    if ( !j.is_string() ) return;

    const auto& value = j.get_ref<const std::string&>();
    if ( value == "NearCursor" ) {
        mode = BeatLineDisplayMode::NearCursor;
    } else if ( value == "Hidden" ) {
        mode = BeatLineDisplayMode::Hidden;
    }
}

void to_json(nlohmann::json& j, const BackgroundFillMode& mode)
{
    switch ( mode ) {
    case BackgroundFillMode::Stretch: j = "Stretch"; break;
    case BackgroundFillMode::AspectFit: j = "AspectFit"; break;
    case BackgroundFillMode::AspectFill: j = "AspectFill"; break;
    case BackgroundFillMode::Center: j = "Center"; break;
    }
}

void from_json(const nlohmann::json& j, BackgroundFillMode& mode)
{
    mode = BackgroundFillMode::Stretch;
    if ( !j.is_string() ) return;

    const auto& value = j.get_ref<const std::string&>();
    if ( value == "AspectFit" ) {
        mode = BackgroundFillMode::AspectFit;
    } else if ( value == "AspectFill" ) {
        mode = BackgroundFillMode::AspectFill;
    } else if ( value == "Center" ) {
        mode = BackgroundFillMode::Center;
    }
}

void to_json(nlohmann::json& j, const BackgroundSpectrumConfig& config)
{
    j = nlohmann::json{ { "enabled", config.enabled },
                        { "bandCount", config.bandCount },
                        { "widthRatio", config.widthRatio },
                        { "heightRatio", config.heightRatio },
                        { "baselineRatio", config.baselineRatio },
                        { "opacity", config.opacity },
                        { "includeHitEffects", config.includeHitEffects } };
}

void from_json(const nlohmann::json& j, BackgroundSpectrumConfig& config)
{
    const BackgroundSpectrumConfig defaults;
    config.enabled   = j.value("enabled", defaults.enabled);
    config.bandCount = std::clamp(j.value("bandCount", defaults.bandCount),
                                  BACKGROUND_SPECTRUM_MIN_BANDS,
                                  BACKGROUND_SPECTRUM_MAX_BANDS);
    config.widthRatio =
        std::clamp(j.value("widthRatio", defaults.widthRatio), 0.10f, 1.0f);
    config.heightRatio =
        std::clamp(j.value("heightRatio", defaults.heightRatio), 0.05f, 1.0f);
    config.baselineRatio = std::clamp(
        j.value("baselineRatio", defaults.baselineRatio), 0.05f, 1.0f);
    config.opacity =
        std::clamp(j.value("opacity", defaults.opacity), 0.0f, 1.0f);
    config.includeHitEffects =
        j.value("includeHitEffects", defaults.includeHitEffects);
}

void to_json(nlohmann::json& j, const BackgroundConfig& config)
{
    j = nlohmann::json{ { "fillMode", config.fillMode },
                        { "darken_ratio", config.darken_ratio },
                        { "opaque_ratio", config.opaque_ratio },
                        { "spectrum", config.spectrum } };
}

void from_json(const nlohmann::json& j, BackgroundConfig& config)
{
    config.fillMode     = j.value("fillMode", BackgroundFillMode::AspectFill);
    config.darken_ratio = j.value("darken_ratio", 0.7f);
    config.opaque_ratio = j.value("opaque_ratio", 1.0f);
    config.spectrum     = j.value("spectrum", BackgroundSpectrumConfig());
}

void to_json(nlohmann::json& j, const CanvasComponentPlacement& placement)
{
    j = nlohmann::json{ { "visible", placement.visible },
                        { "anchorX", placement.anchorX },
                        { "anchorY", placement.anchorY },
                        { "fontSizeRatio", placement.fontSizeRatio },
                        { "color", placement.color } };
}

void from_json(const nlohmann::json& j, CanvasComponentPlacement& placement)
{
    placement.visible       = j.value("visible", false);
    placement.anchorX       = j.value("anchorX", 0.5f);
    placement.anchorY       = j.value("anchorY", 0.12f);
    placement.fontSizeRatio = j.value("fontSizeRatio", 0.035f);
    placement.color =
        j.value("color", std::array<float, 4>{ 1.0f, 1.0f, 1.0f, 1.0f });
}

namespace
{

/// @brief 按当前轨道布局生成单轨 KPS 的默认位置。
/// @param trackIndex 从零开始的轨道序号。
/// @param trackCount 当前轨道总数。
/// @param trackLeft 轨道区域左边界比例。
/// @param trackRight 轨道区域右边界比例。
/// @param color KPS 整组颜色。
/// @return 位于对应轨道中心上方的默认布局。
/// @warning 渲染热路径：未自定义的逐轨 KPS 每帧调用；只做常数次数值计算。
CanvasComponentPlacement defaultKpsTrackPlacement(
    std::int32_t trackIndex, std::int32_t trackCount, float trackLeft,
    float trackRight, const std::array<float, 4>& color)
{
    const auto safeTrackCount = std::max(trackCount, 1);
    const auto safeTrackIndex = std::clamp(trackIndex, 0, safeTrackCount - 1);
    trackLeft                 = std::clamp(trackLeft, 0.0f, 1.0f);
    trackRight                = std::clamp(trackRight, trackLeft, 1.0f);

    CanvasComponentPlacement result;
    result.visible = true;
    result.anchorX = trackLeft + (static_cast<float>(safeTrackIndex) + 0.5f) /
                                     static_cast<float>(safeTrackCount) *
                                     (trackRight - trackLeft);
    result.anchorY = 0.15f;
    result.fontSizeRatio =
        std::clamp(0.044f / std::sqrt(static_cast<float>(safeTrackCount)),
                   0.0125f,
                   0.035f);
    result.color = color;
    return result;
}

/// @brief 恢复组件的默认位置和尺寸，并保留显隐与颜色。
/// @param placement 需要复位的组件布局。
/// @param defaultPlacement 对应组件的默认布局。
void resetPlacementGeometry(CanvasComponentPlacement&       placement,
                            const CanvasComponentPlacement& defaultPlacement)
{
    placement.anchorX       = defaultPlacement.anchorX;
    placement.anchorY       = defaultPlacement.anchorY;
    placement.fontSizeRatio = defaultPlacement.fontSizeRatio;
}

}  // namespace

CanvasComponentPlacement CanvasComponentLayoutConfig::resolvedPlacement(
    CanvasComponentType type, std::int64_t instanceIndex,
    std::int32_t trackCount, float trackLeft, float trackRight) const
{
    if ( type != CanvasComponentType::Kps ||
         instanceIndex == KPS_TOTAL_INSTANCE_INDEX ) {
        return placement(type);
    }

    if ( instanceIndex > std::numeric_limits<std::int32_t>::max() ) {
        return kps;
    }
    const auto trackIndex         = static_cast<std::int32_t>(instanceIndex);
    const auto automaticPlacement = defaultKpsTrackPlacement(
        trackIndex, trackCount, trackLeft, trackRight, kps.color);
    const bool hasSynchronizedFontSize =
        std::isfinite(kpsTrackFontSizeRatio) && kpsTrackFontSizeRatio > 0.0f;
    const float synchronizedFontSize =
        hasSynchronizedFontSize
            ? std::clamp(kpsTrackFontSizeRatio, 0.0125f, 0.25f)
            : automaticPlacement.fontSizeRatio;
    CanvasComponentPlacement result = automaticPlacement;
    if ( hasSynchronizedFontSize ) {
        result.fontSizeRatio = synchronizedFontSize;
    }
    const auto stored =
        std::lower_bound(kpsTracks.begin(),
                         kpsTracks.end(),
                         trackIndex,
                         [](const auto& entry, std::int32_t index) {
                             return entry.trackIndex < index;
                         });
    if ( stored != kpsTracks.end() && stored->trackIndex == trackIndex ) {
        result = stored->placement;
    }
    if ( syncKpsTrackSizes ) {
        result.fontSizeRatio = synchronizedFontSize;
    }
    result.visible = kps.visible;
    result.color   = kps.color;
    return result;
}

CanvasComponentPlacement& CanvasComponentLayoutConfig::editablePlacement(
    CanvasComponentType type, std::int64_t instanceIndex,
    std::int32_t trackCount, float trackLeft, float trackRight)
{
    if ( type != CanvasComponentType::Kps ||
         instanceIndex == KPS_TOTAL_INSTANCE_INDEX || instanceIndex < 0 ) {
        return placement(type);
    }

    if ( instanceIndex > std::numeric_limits<std::int32_t>::max() ) {
        return kps;
    }
    const auto trackIndex = static_cast<std::int32_t>(instanceIndex);
    const auto stored =
        std::lower_bound(kpsTracks.begin(),
                         kpsTracks.end(),
                         trackIndex,
                         [](const auto& entry, std::int32_t index) {
                             return entry.trackIndex < index;
                         });
    if ( stored != kpsTracks.end() && stored->trackIndex == trackIndex ) {
        return stored->placement;
    }

    auto newPlacement = defaultKpsTrackPlacement(
        trackIndex, trackCount, trackLeft, trackRight, kps.color);
    if ( std::isfinite(kpsTrackFontSizeRatio) &&
         kpsTrackFontSizeRatio > 0.0f ) {
        newPlacement.fontSizeRatio =
            std::clamp(kpsTrackFontSizeRatio, 0.0125f, 0.25f);
    }
    return kpsTracks.insert(stored, { trackIndex, newPlacement })->placement;
}

void CanvasComponentLayoutConfig::synchronizeKpsTrackFontSize(
    float fontSizeRatio)
{
    if ( !std::isfinite(fontSizeRatio) ) return;
    kpsTrackFontSizeRatio = std::clamp(fontSizeRatio, 0.0125f, 0.25f);
    for ( auto& track : kpsTracks ) {
        track.placement.fontSizeRatio = kpsTrackFontSizeRatio;
    }
}

void CanvasComponentLayoutConfig::setSyncKpsTrackRelativePositions(bool enabled)
{
    syncKpsTrackRelativePositions = enabled;
    if ( enabled ) {
        syncAllKpsComponentPositions = false;
    }
}

void CanvasComponentLayoutConfig::setSyncAllKpsComponentPositions(bool enabled)
{
    syncAllKpsComponentPositions = enabled;
    if ( enabled ) {
        syncKpsTrackRelativePositions = false;
    }
}

void CanvasComponentLayoutConfig::resetPlacementToDefault(
    CanvasComponentType type)
{
    switch ( type ) {
    case CanvasComponentType::JudgmentLineTime:
        resetPlacementGeometry(judgmentLineTime,
                               DEFAULT_JUDGMENT_LINE_TIME_PLACEMENT);
        break;
    case CanvasComponentType::BeatNumber:
        resetPlacementGeometry(beatNumber, DEFAULT_BEAT_NUMBER_PLACEMENT);
        break;
    case CanvasComponentType::BeatLineTime:
        resetPlacementGeometry(beatLineTime, DEFAULT_BEAT_LINE_TIME_PLACEMENT);
        break;
    case CanvasComponentType::Kps:
        resetPlacementGeometry(kps, DEFAULT_KPS_TOTAL_PLACEMENT);
        kpsTracks.clear();
        kpsTrackFontSizeRatio = 0.0f;
        break;
    case CanvasComponentType::BackgroundSpectrum:
        resetPlacementGeometry(backgroundSpectrum,
                               DEFAULT_BACKGROUND_SPECTRUM_PLACEMENT);
        break;
    case CanvasComponentType::Count: break;
    }
}

void to_json(nlohmann::json& j, const CanvasKpsTrackPlacement& placement)
{
    j = nlohmann::json{ { "trackIndex", placement.trackIndex },
                        { "placement", placement.placement } };
}

void from_json(const nlohmann::json& j, CanvasKpsTrackPlacement& placement)
{
    placement.trackIndex = j.value("trackIndex", 0);
    placement.placement  = j.value("placement", CanvasComponentPlacement{});
}

void to_json(nlohmann::json& j, const CanvasComponentLayoutConfig& config)
{
    j = nlohmann::json{
        { "judgmentLineTime", config.judgmentLineTime },
        { "beatNumber", config.beatNumber },
        { "beatLineTime", config.beatLineTime },
        { "fontSizeUsesCanvasHeight", true },
        { "kps", config.kps },
        { "backgroundSpectrum", config.backgroundSpectrum },
        { "kpsTracks", config.kpsTracks },
        { "syncKpsTrackSizes", config.syncKpsTrackSizes },
        { "syncKpsTrackRelativePositions",
          config.syncKpsTrackRelativePositions },
        { "syncAllKpsComponentPositions", config.syncAllKpsComponentPositions },
        { "kpsTrackFontSizeRatio", config.kpsTrackFontSizeRatio }
    };
}

void from_json(const nlohmann::json& j, CanvasComponentLayoutConfig& config)
{
    config.judgmentLineTime =
        j.value("judgmentLineTime", CanvasComponentPlacement());
    config.beatNumber = j.value("beatNumber", DEFAULT_BEAT_NUMBER_PLACEMENT);
    config.beatLineTime =
        j.value("beatLineTime", DEFAULT_BEAT_LINE_TIME_PLACEMENT);
    const bool fontSizeUsesCanvasHeight =
        j.value("fontSizeUsesCanvasHeight", false);
    if ( !fontSizeUsesCanvasHeight && j.contains("beatNumber") ) {
        config.beatNumber.fontSizeRatio =
            std::clamp(config.beatNumber.fontSizeRatio *
                           (DEFAULT_BEAT_NUMBER_PLACEMENT.fontSizeRatio /
                            LEGACY_REPEATED_TEXT_FONT_SIZE_RATIO),
                       0.0125f,
                       0.25f);
    }
    if ( !fontSizeUsesCanvasHeight && j.contains("beatLineTime") ) {
        config.beatLineTime.fontSizeRatio =
            std::clamp(config.beatLineTime.fontSizeRatio *
                           (DEFAULT_BEAT_LINE_TIME_PLACEMENT.fontSizeRatio /
                            LEGACY_REPEATED_TEXT_FONT_SIZE_RATIO),
                       0.0125f,
                       0.25f);
    }
    config.kps = j.value("kps", DEFAULT_KPS_TOTAL_PLACEMENT);
    config.backgroundSpectrum =
        j.value("backgroundSpectrum", DEFAULT_BACKGROUND_SPECTRUM_PLACEMENT);
    config.kpsTracks =
        j.value("kpsTracks", std::vector<CanvasKpsTrackPlacement>{});
    config.syncKpsTrackSizes = j.value("syncKpsTrackSizes", false);
    config.syncKpsTrackRelativePositions =
        j.value("syncKpsTrackRelativePositions", false);
    config.setSyncAllKpsComponentPositions(
        j.value("syncAllKpsComponentPositions", false));
    config.kpsTrackFontSizeRatio = j.value("kpsTrackFontSizeRatio", 0.0f);
    if ( !std::isfinite(config.kpsTrackFontSizeRatio) ||
         config.kpsTrackFontSizeRatio <= 0.0f ) {
        config.kpsTrackFontSizeRatio = 0.0f;
    } else {
        config.kpsTrackFontSizeRatio =
            std::clamp(config.kpsTrackFontSizeRatio, 0.0125f, 0.25f);
    }
    std::erase_if(config.kpsTracks, [](const auto& placement) {
        return placement.trackIndex < 0;
    });
    std::stable_sort(config.kpsTracks.begin(),
                     config.kpsTracks.end(),
                     [](const auto& lhs, const auto& rhs) {
                         return lhs.trackIndex < rhs.trackIndex;
                     });
    config.kpsTracks.erase(std::unique(config.kpsTracks.begin(),
                                       config.kpsTracks.end(),
                                       [](const auto& lhs, const auto& rhs) {
                                           return lhs.trackIndex ==
                                                  rhs.trackIndex;
                                       }),
                           config.kpsTracks.end());
}

void to_json(nlohmann::json& j, const PreviewAreaConfig::AreaMargin& margin)
{
    j = nlohmann::json{ { "left", margin.left },
                        { "top", margin.top },
                        { "right", margin.right },
                        { "bottom", margin.bottom } };
}

void from_json(const nlohmann::json& j, PreviewAreaConfig::AreaMargin& margin)
{
    margin.left   = j.value("left", 4.0f);
    margin.top    = j.value("top", 4.0f);
    margin.right  = j.value("right", 4.0f);
    margin.bottom = j.value("bottom", 4.0f);
}

void to_json(nlohmann::json& j, const PreviewAreaConfig& config)
{
    j = nlohmann::json{ { "areaRatio", config.areaRatio },
                        { "edgeScrollSensitivity",
                          config.edgeScrollSensitivity },
                        { "margin", config.margin },
                        { "drawBeatLines", config.drawBeatLines },
                        { "drawTimingLines", config.drawTimingLines } };
}

void from_json(const nlohmann::json& j, PreviewAreaConfig& config)
{
    config.areaRatio             = j.value("areaRatio", 5.0f);
    config.edgeScrollSensitivity = j.value("edgeScrollSensitivity", 1.0f);
    config.margin          = j.value("margin", PreviewAreaConfig::AreaMargin());
    config.drawBeatLines   = j.value("drawBeatLines", true);
    config.drawTimingLines = j.value("drawTimingLines", true);
}

void to_json(nlohmann::json& j, const SpectrumDetailLevel& level)
{
    switch ( level ) {
    case SpectrumDetailLevel::Performance: j = "Performance"; break;
    case SpectrumDetailLevel::Balanced: j = "Balanced"; break;
    case SpectrumDetailLevel::Fine: j = "Fine"; break;
    case SpectrumDetailLevel::Ultra: j = "Ultra"; break;
    case SpectrumDetailLevel::Extreme: j = "Extreme"; break;
    case SpectrumDetailLevel::Experimental: j = "Experimental"; break;
    }
}

void from_json(const nlohmann::json& j, SpectrumDetailLevel& level)
{
    level = SpectrumDetailLevel::Performance;
    if ( !j.is_string() ) return;

    const auto& value = j.get_ref<const std::string&>();
    if ( value == "Balanced" ) {
        level = SpectrumDetailLevel::Balanced;
    } else if ( value == "Fine" ) {
        level = SpectrumDetailLevel::Fine;
    } else if ( value == "Ultra" ) {
        level = SpectrumDetailLevel::Ultra;
    } else if ( value == "Extreme" ) {
        level = SpectrumDetailLevel::Extreme;
    } else if ( value == "Experimental" ) {
        level = SpectrumDetailLevel::Experimental;
    }
}

void to_json(nlohmann::json& j, const TrackLayout& layout)
{
    j = nlohmann::json{ { "left", layout.left },
                        { "top", layout.top },
                        { "right", layout.right },
                        { "bottom", layout.bottom } };
}

void from_json(const nlohmann::json& j, TrackLayout& layout)
{
    layout.left   = j.value("left", 0.2f);
    layout.top    = j.value("top", 0.05f);
    layout.right  = j.value("right", 0.8f);
    layout.bottom = j.value("bottom", 0.95f);
}

void to_json(nlohmann::json& j, const VisualConfig& config)
{
    auto background = config.background;
    background.spectrum.enabled =
        config.canvasComponents.backgroundSpectrum.visible;
    j = nlohmann::json{
        { "trackLayout", config.trackLayout },
        { "canvasComponents", config.canvasComponents },
        { "background", background },
        { "previewConfig", config.previewConfig },
        { "trackBoxLineWidth", config.trackBoxLineWidth },
        { "judgeline_pos", config.judgeline_pos },
        { "noteScaleX", config.noteScaleX },
        { "noteScaleY", config.noteScaleY },
        { "noteFillMode", config.noteFillMode },
        { "visualOffset", config.visualOffset },
        { "waveformVisualOffset", config.waveformVisualOffset },
        { "spectrumVisualOffset", config.spectrumVisualOffset },
        { "timelineZoom", config.timelineZoom },
        { "scrollAnimationDuration", config.scrollAnimationDuration },
        { "enableLinearScrollMapping", config.enableLinearScrollMapping },
        { "snapThreshold", config.snapThreshold },
        { "beatLineAlpha", config.beatLineAlpha },
        { "beatLineDisplayMode", config.beatLineDisplayMode },
        { "beatLineCursorVisibleRatio", config.beatLineCursorVisibleRatio },
        { "beatLineCursorFadeRatio", config.beatLineCursorFadeRatio },
        { "drawBeatLinesBeforeFirstTiming",
          config.drawBeatLinesBeforeFirstTiming },
        { "drawBeatLines",
          config.beatLineDisplayMode != BeatLineDisplayMode::Hidden },
        { "spectrumDetailLevel", config.spectrumDetailLevel },
        { "enableHitEffects", config.enableHitEffects },
        { "debugDrawHitboxes", config.debugDrawHitboxes }
    };
}

void from_json(const nlohmann::json& j, VisualConfig& config)
{
    const bool hasBackgroundSpectrumComponent =
        j.contains("canvasComponents") &&
        j.at("canvasComponents").is_object() &&
        j.at("canvasComponents").contains("backgroundSpectrum");
    config.trackLayout = j.value("trackLayout", TrackLayout());
    config.canvasComponents =
        j.value("canvasComponents", CanvasComponentLayoutConfig());
    config.background = j.value("background", BackgroundConfig());
    if ( !hasBackgroundSpectrumComponent ) {
        config.canvasComponents.backgroundSpectrum.visible =
            config.background.spectrum.enabled;
        config.canvasComponents.backgroundSpectrum.anchorY =
            std::clamp(config.background.spectrum.baselineRatio -
                           config.background.spectrum.heightRatio * 0.5f,
                       0.0f,
                       1.0f);
    }
    config.background.spectrum.enabled =
        config.canvasComponents.backgroundSpectrum.visible;
    config.previewConfig     = j.value("previewConfig", PreviewAreaConfig());
    config.trackBoxLineWidth = j.value("trackBoxLineWidth", 1.5f);
    config.judgeline_pos     = j.value("judgeline_pos", 0.85f);
    config.noteScaleX        = j.value("noteScaleX", VisualConfig{}.noteScaleX);
    config.noteScaleY        = j.value("noteScaleY", VisualConfig{}.noteScaleY);
    config.noteFillMode = j.value("noteFillMode", BackgroundFillMode::Stretch);
    config.visualOffset = j.value("visualOffset", 0.0f);
    config.waveformVisualOffset    = j.value("waveformVisualOffset", 0.0f);
    config.spectrumVisualOffset    = j.value("spectrumVisualOffset", 0.0f);
    config.timelineZoom            = j.value("timelineZoom", 1.0f);
    config.scrollAnimationDuration = j.value("scrollAnimationDuration", 0.12f);
    config.enableLinearScrollMapping =
        j.value("enableLinearScrollMapping", false);
    config.snapThreshold = j.value("snapThreshold", 16.0f);
    config.beatLineAlpha = j.value("beatLineAlpha", 0.75f);
    if ( j.contains("beatLineDisplayMode") ) {
        config.beatLineDisplayMode =
            j.value("beatLineDisplayMode", BeatLineDisplayMode::Always);
    } else {
        config.beatLineDisplayMode = j.value("drawBeatLines", true)
                                         ? BeatLineDisplayMode::Always
                                         : BeatLineDisplayMode::Hidden;
    }
    config.beatLineCursorVisibleRatio =
        std::clamp(j.value("beatLineCursorVisibleRatio", 0.16f), 0.05f, 0.50f);
    config.beatLineCursorFadeRatio =
        std::clamp(j.value("beatLineCursorFadeRatio", 0.20f), 0.02f, 0.40f);
    config.overrideBeatLineColors = false;
    config.beatLineColors         = {};
    config.drawBeatLinesBeforeFirstTiming =
        j.value("drawBeatLinesBeforeFirstTiming", true);
    config.spectrumDetailLevel =
        j.value("spectrumDetailLevel", SpectrumDetailLevel::Balanced);
    config.enableHitEffects  = j.value("enableHitEffects", true);
    config.debugDrawHitboxes = j.value("debugDrawHitboxes", false);
}

}  // namespace MMM::Config
