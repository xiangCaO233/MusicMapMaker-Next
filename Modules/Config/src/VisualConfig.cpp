#include "config/VisualConfig.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>

namespace MMM::Config
{

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

void to_json(nlohmann::json& j, const BackgroundConfig& config)
{
    j = nlohmann::json{ { "fillMode", config.fillMode },
                        { "darken_ratio", config.darken_ratio },
                        { "opaque_ratio", config.opaque_ratio } };
}

void from_json(const nlohmann::json& j, BackgroundConfig& config)
{
    config.fillMode     = j.value("fillMode", BackgroundFillMode::AspectFill);
    config.darken_ratio = j.value("darken_ratio", 0.7f);
    config.opaque_ratio = j.value("opaque_ratio", 1.0f);
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

void to_json(nlohmann::json& j, const CanvasComponentLayoutConfig& config)
{
    j = nlohmann::json{ { "judgmentLineTime", config.judgmentLineTime },
                        { "beatNumber", config.beatNumber },
                        { "beatLineTime", config.beatLineTime } };
}

void from_json(const nlohmann::json& j, CanvasComponentLayoutConfig& config)
{
    config.judgmentLineTime =
        j.value("judgmentLineTime", CanvasComponentPlacement());
    config.beatNumber = j.value("beatNumber", DEFAULT_BEAT_NUMBER_PLACEMENT);
    config.beatLineTime =
        j.value("beatLineTime", DEFAULT_BEAT_LINE_TIME_PLACEMENT);
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
    j = nlohmann::json{
        { "trackLayout", config.trackLayout },
        { "canvasComponents", config.canvasComponents },
        { "background", config.background },
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
    config.trackLayout = j.value("trackLayout", TrackLayout());
    config.canvasComponents =
        j.value("canvasComponents", CanvasComponentLayoutConfig());
    config.background        = j.value("background", BackgroundConfig());
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
