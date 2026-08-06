#include "config/CreatorIdentity.h"
#include "config/EditorConfig.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace MMM::Config
{
/// @brief 将持久化打击音效增益限制为音频控制层支持的有限范围。
/// @param gain 配置文件读取的线性增益。
/// @return 0.0~2.0 的有限值；非有限输入按静音增益处理。
float sanitizeHitSfxGain(float gain) noexcept
{
    return std::isfinite(gain) ? std::clamp(gain, 0.0F, 2.0F) : 0.0F;
}

void to_json(nlohmann::json& json, const SyncMode& mode)
{
    json = "None";
    switch ( mode ) {
    case SyncMode::None: json = "None"; break;
    case SyncMode::Integral: json = "Integral"; break;
    case SyncMode::WaterTank: json = "WaterTank"; break;
    }
}

void from_json(const nlohmann::json& json, SyncMode& mode)
{
    mode = SyncMode::None;
    if ( !json.is_string() ) return;
    const auto value = json.get<std::string>();
    if ( value == "Integral" ) {
        mode = SyncMode::Integral;
    } else if ( value == "WaterTank" ) {
        mode = SyncMode::WaterTank;
    }
}

void to_json(nlohmann::json& json, const SyncConfig& config)
{
    json = nlohmann::json{ { "mode", config.mode },
                           { "integralFactor", config.integralFactor },
                           { "waterTankBuffer", config.waterTankBuffer },
                           { "syncInterval", config.syncInterval } };
}

void from_json(const nlohmann::json& json, SyncConfig& config)
{
    config.mode            = json.value("mode", SyncMode::Integral);
    config.integralFactor  = json.value("integralFactor", 0.1f);
    config.waterTankBuffer = json.value("waterTankBuffer", 0.05f);
    config.syncInterval    = json.value("syncInterval", 10.0);
}

void to_json(nlohmann::json& json, const PolylineSfxStrategy& strategy)
{
    json = "Exact";
    switch ( strategy ) {
    case PolylineSfxStrategy::Exact: json = "Exact"; break;
    case PolylineSfxStrategy::InternalAsNormal:
        json = "InternalAsNormal";
        break;
    case PolylineSfxStrategy::OnlyTailExact: json = "OnlyTailExact"; break;
    case PolylineSfxStrategy::AllAsNormal: json = "AllAsNormal"; break;
    }
}

void from_json(const nlohmann::json& json, PolylineSfxStrategy& strategy)
{
    strategy = PolylineSfxStrategy::Exact;
    if ( !json.is_string() ) return;
    const auto value = json.get<std::string>();
    if ( value == "InternalAsNormal" ) {
        strategy = PolylineSfxStrategy::InternalAsNormal;
    } else if ( value == "OnlyTailExact" ) {
        strategy = PolylineSfxStrategy::OnlyTailExact;
    } else if ( value == "AllAsNormal" ) {
        strategy = PolylineSfxStrategy::AllAsNormal;
    }
}

void to_json(nlohmann::json& json, const SfxConfig& config)
{
    json = nlohmann::json{
        { "polylineStrategy", config.polylineStrategy },
        { "enableFlickWidthVolumeScaling",
          config.enableFlickWidthVolumeScaling },
        { "flickWidthVolumeMultiplier", config.flickWidthVolumeMultiplier },
        { "enableStereoHitEffects", config.enableStereoHitEffects },
        { "permanentSfxVolumes", config.permanentSfxVolumes },
        { "permanentSfxMutes", config.permanentSfxMutes },
        { "hitSfxSyncSpeed", config.hitSfxSyncSpeed },
        { "enableHitSfx", config.enableHitSfx },
        { "enableUnboundHitSfx", config.enableUnboundHitSfx },
        { "unboundHitSfxGain", sanitizeHitSfxGain(config.unboundHitSfxGain) },
        { "enableBoundHitSfx", config.enableBoundHitSfx },
        { "boundHitSfxGain", sanitizeHitSfxGain(config.boundHitSfxGain) }
    };
}

void from_json(const nlohmann::json& json, SfxConfig& config)
{
    config.polylineStrategy =
        json.value("polylineStrategy", PolylineSfxStrategy::Exact);
    config.enableFlickWidthVolumeScaling =
        json.value("enableFlickWidthVolumeScaling", false);
    config.flickWidthVolumeMultiplier =
        json.value("flickWidthVolumeMultiplier", 0.1f);
    config.enableStereoHitEffects =
        json.value("enableStereoHitEffects",
                   json.value("enableDirectionalFlickChannels", true));
    config.permanentSfxVolumes =
        json.value("permanentSfxVolumes", std::map<std::string, float>());
    config.permanentSfxMutes =
        json.value("permanentSfxMutes", std::map<std::string, bool>());
    config.hitSfxSyncSpeed     = json.value("hitSfxSyncSpeed", true);
    config.enableHitSfx        = json.value("enableHitSfx", true);
    config.enableUnboundHitSfx = json.value("enableUnboundHitSfx", true);
    config.unboundHitSfxGain =
        sanitizeHitSfxGain(json.value("unboundHitSfxGain", 1.0F));
    config.enableBoundHitSfx = json.value("enableBoundHitSfx", true);
    config.boundHitSfxGain =
        sanitizeHitSfxGain(json.value("boundHitSfxGain", 1.0F));
}

void to_json(nlohmann::json& json, const FilePickerStyle& style)
{
    json = style == FilePickerStyle::Unified ? "Unified" : "Native";
}

void from_json(const nlohmann::json& json, FilePickerStyle& style)
{
    style = FilePickerStyle::Native;
    if ( json.is_string() && json.get<std::string>() == "Unified" ) {
        style = FilePickerStyle::Unified;
    }
}

void to_json(nlohmann::json& json, const CursorStyle& style)
{
    json = style == CursorStyle::System ? "System" : "Software";
}

void from_json(const nlohmann::json& json, CursorStyle& style)
{
    style = CursorStyle::Software;
    if ( json.is_string() && json.get<std::string>() == "System" ) {
        style = CursorStyle::System;
    }
}

void to_json(nlohmann::json& json, const SoftwareCursorConfig& config)
{
    json = nlohmann::json{ { "cursorSize", config.cursorSize },
                           { "trailSize", config.trailSize },
                           { "trailLifeTime", config.trailLifeTime },
                           { "smokeSize", config.smokeSize },
                           { "smokeLifeTime", config.smokeLifeTime },
                           { "enableBpmSyncSmokeLife",
                             config.enableBpmSyncSmokeLife } };
}

void from_json(const nlohmann::json& json, SoftwareCursorConfig& config)
{
    config.cursorSize             = json.value("cursorSize", 64.0f);
    config.trailSize              = json.value("trailSize", 48.0f);
    config.trailLifeTime          = json.value("trailLifeTime", 0.4f);
    config.smokeSize              = json.value("smokeSize", 32.0f);
    config.smokeLifeTime          = json.value("smokeLifeTime", 0.8f);
    config.enableBpmSyncSmokeLife = json.value("enableBpmSyncSmokeLife", false);
}

void to_json(nlohmann::json& json, const UIAestheticsConfig& config)
{
    json = nlohmann::json{ { "windowRounding", config.windowRounding },
                           { "frameRounding", config.frameRounding },
                           { "windowGap", config.windowGap },
                           { "itemSpacing", config.itemSpacing },
                           { "windowPadding", config.windowPadding },
                           { "animationTransitionDuration",
                             config.animationTransitionDuration } };
}

void from_json(const nlohmann::json& json, UIAestheticsConfig& config)
{
    config.windowRounding = json.value("windowRounding", 8.0f);
    config.frameRounding  = json.value("frameRounding", 6.0f);
    config.windowGap      = json.value("windowGap", 8.0f);
    config.itemSpacing    = json.value("itemSpacing", 8.0f);
    config.windowPadding  = json.value("windowPadding", 8.0f);
    config.animationTransitionDuration =
        std::max(UIAestheticsConfig::MIN_ANIMATION_TRANSITION_DURATION,
                 json.value("animationTransitionDuration", 0.12f));
}

void to_json(nlohmann::json& json, const ColorPaletteScheme& scheme)
{
    json = nlohmann::json{ { "name", scheme.name },
                           { "noteColors", scheme.noteColors },
                           { "beatLineColors", scheme.beatLineColors } };
}

void from_json(const nlohmann::json& json, ColorPaletteScheme& scheme)
{
    scheme.name       = json.value("name", std::string("Palette"));
    scheme.noteColors = json.value("noteColors", decltype(scheme.noteColors){});
    scheme.beatLineColors =
        json.value("beatLineColors", BeatLineColorPalette{});
}

void to_json(nlohmann::json& json, const ColorPaletteConfig& config)
{
    json = nlohmann::json{ { "activeSchemeIndex", config.activeSchemeIndex },
                           { "schemes", config.schemes } };
}

void from_json(const nlohmann::json& json, ColorPaletteConfig& config)
{
    config.activeSchemeIndex =
        json.value("activeSchemeIndex", std::size_t{ 0 });
    config.schemes = json.value("schemes", std::vector<ColorPaletteScheme>());
    if ( config.schemes.empty() ||
         config.activeSchemeIndex >= config.schemes.size() ) {
        config.activeSchemeIndex = 0;
    }
}

void to_json(nlohmann::json& json, const ShortcutBinding& binding)
{
    json = nlohmann::json{
        { "enabled", binding.enabled }, { "key", binding.key },
        { "ctrl", binding.ctrl },       { "shift", binding.shift },
        { "alt", binding.alt },         { "super", binding.super }
    };
}

void from_json(const nlohmann::json& json, ShortcutBinding& binding)
{
    binding.enabled = json.value("enabled", true);
    binding.key     = json.value("key", std::string());
    binding.ctrl    = json.value("ctrl", false);
    binding.shift   = json.value("shift", false);
    binding.alt     = json.value("alt", false);
    binding.super   = json.value("super", false);
    if ( binding.key.empty() ) binding.enabled = false;
}

void to_json(nlohmann::json& json, const ShortcutConfig& config)
{
    json = nlohmann::json{
        { "toolMove", config.toolMove },
        { "toolMarquee", config.toolMarquee },
        { "toolDraw", config.toolDraw },
        { "toolColorBrush", config.toolColorBrush },
        { "toolColorEraser", config.toolColorEraser },
        { "mirror", config.mirror },
        { "mirrorPaste", config.mirrorPaste },
        { "deleteSelected", config.deleteSelected },
        { "toggleReverseScroll", config.toggleReverseScroll },
        { "toggleScrollSnap", config.toggleScrollSnap },
        { "toggleSnapFloor", config.toggleSnapFloor },
        { "toggleScrollTimingMapping", config.toggleScrollTimingMapping },
        { "toggleBeatLines", config.toggleBeatLines },
        { "toggleStopPlaybackOnScroll", config.toggleStopPlaybackOnScroll },
        { "toggleHitSfx", config.toggleHitSfx },
        { "toggleHitEffects", config.toggleHitEffects },
        { "toggleSyncSameMainAudio", config.toggleSyncSameMainAudio }
    };
}

void from_json(const nlohmann::json& json, ShortcutConfig& config)
{
    const ShortcutConfig defaults;
    config.toolMove    = json.value("toolMove", defaults.toolMove);
    config.toolMarquee = json.value("toolMarquee", defaults.toolMarquee);
    config.toolDraw    = json.value("toolDraw", defaults.toolDraw);
    config.toolColorBrush =
        json.value("toolColorBrush", defaults.toolColorBrush);
    config.toolColorEraser =
        json.value("toolColorEraser", defaults.toolColorEraser);
    config.mirror      = json.value("mirror", defaults.mirror);
    config.mirrorPaste = json.value("mirrorPaste", defaults.mirrorPaste);
    config.deleteSelected =
        json.value("deleteSelected", defaults.deleteSelected);
    config.toggleReverseScroll =
        json.value("toggleReverseScroll", defaults.toggleReverseScroll);
    config.toggleScrollSnap =
        json.value("toggleScrollSnap", defaults.toggleScrollSnap);
    config.toggleSnapFloor =
        json.value("toggleSnapFloor", defaults.toggleSnapFloor);
    config.toggleScrollTimingMapping = json.value(
        "toggleScrollTimingMapping", defaults.toggleScrollTimingMapping);
    config.toggleBeatLines =
        json.value("toggleBeatLines", defaults.toggleBeatLines);
    config.toggleStopPlaybackOnScroll = json.value(
        "toggleStopPlaybackOnScroll", defaults.toggleStopPlaybackOnScroll);
    config.toggleHitSfx = json.value("toggleHitSfx", defaults.toggleHitSfx);
    config.toggleHitEffects =
        json.value("toggleHitEffects", defaults.toggleHitEffects);
    config.toggleSyncSameMainAudio =
        json.value("toggleSyncSameMainAudio", defaults.toggleSyncSameMainAudio);
}

void to_json(nlohmann::json& json, const AudioPlaybackBackend& backend)
{
    json = "SDL";
    switch ( backend ) {
    case AudioPlaybackBackend::SDL: json = "SDL"; break;
    case AudioPlaybackBackend::OpenAL: json = "OpenAL"; break;
    }
}

void from_json(const nlohmann::json& json, AudioPlaybackBackend& backend)
{
    backend = AudioPlaybackBackend::SDL;
    if ( !json.is_string() ) return;
    const auto value = json.get<std::string>();
    if ( value == "OpenAL" ) {
        backend = AudioPlaybackBackend::OpenAL;
    } else if ( value == "SDL" ) {
        backend = AudioPlaybackBackend::SDL;
    }
}

void to_json(nlohmann::json& json, const OpenALSpatialConfig& config)
{
    json = nlohmann::json{ { "enabled", config.enabled },
                           { "directionX", config.directionX },
                           { "directionY", config.directionY },
                           { "directionZ", config.directionZ },
                           { "distance", config.distance },
                           { "referenceDistance", config.referenceDistance },
                           { "maxDistance", config.maxDistance },
                           { "rolloffFactor", config.rolloffFactor } };
}

void from_json(const nlohmann::json& json, OpenALSpatialConfig& config)
{
    config.enabled           = json.value("enabled", false);
    config.directionX        = json.value("directionX", 0.0f);
    config.directionY        = json.value("directionY", 0.0f);
    config.directionZ        = json.value("directionZ", -1.0f);
    config.distance          = json.value("distance", 1.0f);
    config.referenceDistance = json.value("referenceDistance", 1.0f);
    config.maxDistance       = json.value("maxDistance", 100.0f);
    config.rolloffFactor     = json.value("rolloffFactor", 1.0f);
}

void to_json(nlohmann::json& json, const FrameLimitPreference& preference)
{
    json = "VSync";
    switch ( preference ) {
    case FrameLimitPreference::VSync: json = "VSync"; break;
    case FrameLimitPreference::Refresh2x: json = "Refresh2x"; break;
    case FrameLimitPreference::Refresh4x: json = "Refresh4x"; break;
    case FrameLimitPreference::Refresh8x: json = "Refresh8x"; break;
    case FrameLimitPreference::Unlimited: json = "Unlimited"; break;
    }
}

void from_json(const nlohmann::json& json, FrameLimitPreference& preference)
{
    preference = FrameLimitPreference::VSync;
    if ( !json.is_string() ) return;
    const auto value = json.get<std::string>();
    if ( value == "VSync" ) {
        preference = FrameLimitPreference::VSync;
    } else if ( value == "Refresh2x" ) {
        preference = FrameLimitPreference::Refresh2x;
    } else if ( value == "Refresh4x" ) {
        preference = FrameLimitPreference::Refresh4x;
    } else if ( value == "Refresh8x" ) {
        preference = FrameLimitPreference::Refresh8x;
    } else if ( value == "Unlimited" ) {
        preference = FrameLimitPreference::Unlimited;
    }
}

void to_json(nlohmann::json& json, const SelectionMode& mode)
{
    json = mode == SelectionMode::Intersection ? "Intersection" : "Strict";
}

void from_json(const nlohmann::json& json, SelectionMode& mode)
{
    mode = SelectionMode::Strict;
    if ( json.is_string() && json.get<std::string>() == "Intersection" ) {
        mode = SelectionMode::Intersection;
    }
}

void to_json(nlohmann::json& json, const SaveFormatPreference& preference)
{
    json =
        preference == SaveFormatPreference::ForceMMM ? "ForceMMM" : "Original";
}

void from_json(const nlohmann::json& json, SaveFormatPreference& preference)
{
    preference = SaveFormatPreference::Original;
    if ( json.is_string() && json.get<std::string>() == "ForceMMM" ) {
        preference = SaveFormatPreference::ForceMMM;
    }
}

void to_json(nlohmann::json& json, const TimeFormatPreference& preference)
{
    json = "Clock";
    switch ( preference ) {
    case TimeFormatPreference::Clock: json = "Clock"; break;
    case TimeFormatPreference::Seconds: json = "Seconds"; break;
    case TimeFormatPreference::Milliseconds: json = "Milliseconds"; break;
    case TimeFormatPreference::Beat: json = "Beat"; break;
    }
}

void from_json(const nlohmann::json& json, TimeFormatPreference& preference)
{
    preference = TimeFormatPreference::Clock;
    if ( !json.is_string() ) return;
    const auto value = json.get<std::string>();
    if ( value == "Seconds" ) {
        preference = TimeFormatPreference::Seconds;
    } else if ( value == "Milliseconds" ) {
        preference = TimeFormatPreference::Milliseconds;
    } else if ( value == "Beat" ) {
        preference = TimeFormatPreference::Beat;
    }
}

void to_json(nlohmann::json& json, const CopyPasteTimeBasis& basis)
{
    json = basis == CopyPasteTimeBasis::Beat ? "Beat" : "Timestamp";
}

void from_json(const nlohmann::json& json, CopyPasteTimeBasis& basis)
{
    basis = CopyPasteTimeBasis::Timestamp;
    if ( json.is_string() && json.get<std::string>() == "Beat" ) {
        basis = CopyPasteTimeBasis::Beat;
    }
}

void to_json(nlohmann::json& json, const ObjectPlacementSnapMode& mode)
{
    json = mode == ObjectPlacementSnapMode::CommonBeatDivisors
               ? "CommonBeatDivisors"
               : "CurrentBeatDivisor";
}

void from_json(const nlohmann::json& json, ObjectPlacementSnapMode& mode)
{
    mode = ObjectPlacementSnapMode::CurrentBeatDivisor;
    if ( json.is_string() && json.get<std::string>() == "CommonBeatDivisors" ) {
        mode = ObjectPlacementSnapMode::CommonBeatDivisors;
    }
}

void to_json(nlohmann::json&                      json,
             const BpmMeasurementToolPreferences& preferences)
{
    json =
        nlohmann::json{ { "markerWidthMs", preferences.markerWidthMs },
                        { "beatDivisor", preferences.beatDivisor },
                        { "viewCenterSeconds", preferences.viewCenterSeconds },
                        { "viewHalfWidthSeconds",
                          preferences.viewHalfWidthSeconds } };
}

void from_json(const nlohmann::json&          json,
               BpmMeasurementToolPreferences& preferences)
{
    preferences.markerWidthMs        = json.value("markerWidthMs", 80.0);
    preferences.beatDivisor          = json.value("beatDivisor", 4);
    preferences.viewCenterSeconds    = json.value("viewCenterSeconds", 0.0);
    preferences.viewHalfWidthSeconds = json.value("viewHalfWidthSeconds", 8.0);
}

void to_json(nlohmann::json& json, const EditorSettings& settings)
{
    json = nlohmann::json{
        { "syncConfig", settings.syncConfig },
        { "sfxConfig", settings.sfxConfig },
        { "filePickerStyle", settings.filePickerStyle },
        { "cursorStyle", settings.cursorStyle },
        { "theme", settings.theme },
        { "disabledPluginIds", settings.disabledPluginIds },
        { "selectedSkinDirectory", settings.selectedSkinDirectory },
        { "beatDivisor", settings.beatDivisor },
        { "overlapTimeWindowMs", settings.overlapTimeWindowMs },
        { "reverseScroll", settings.reverseScroll },
        { "scrollSnap", settings.scrollSnap },
        { "objectPlacementSnap", settings.objectPlacementSnap },
        { "objectPlacementSnapMode", settings.objectPlacementSnapMode },
        { "commonBeatDivisorMask", settings.commonBeatDivisorMask },
        { "recentProjectsLimit", settings.recentProjectsLimit },
        { "language", settings.language },
        { "defaultCreator", normalizeCreatorIdentity(settings.defaultCreator) },
        { "frameLimit", settings.frameLimit },
        { "audioPlaybackBackend", settings.audioPlaybackBackend },
        { "sdlAudioOutputDeviceName", settings.sdlAudioOutputDeviceName },
        { "openALAudioOutputDeviceName", settings.openALAudioOutputDeviceName },
        { "openALSpatialConfig", settings.openALSpatialConfig },
        { "renderProfileLogging", settings.renderProfileLogging },
        { "autoUploadPgoProfiles", settings.autoUploadPgoProfiles },
        { "pgoProfileUploadConsentAsked",
          settings.pgoProfileUploadConsentAsked },
        { "fontSizeMultiplier", settings.fontSizeMultiplier },
        { "uiScaleMultiplier", settings.uiScaleMultiplier },
        { "scrollSpeedMultiplier", settings.scrollSpeedMultiplier },
        { "globalVolume", settings.globalVolume },
        { "globalMuted", settings.globalMuted },
        { "bgmGain", settings.bgmGain },
        { "bgmGainMuted", settings.bgmGainMuted },
        { "sfxGain", settings.sfxGain },
        { "sfxGainMuted", settings.sfxGainMuted },
        { "interactionSfxGain", settings.interactionSfxGain },
        { "interactionSfxGainMuted", settings.interactionSfxGainMuted },
        { "selectionMode", settings.selectionMode },
        { "marqueeThickness", settings.marqueeThickness },
        { "marqueeRounding", settings.marqueeRounding },
        { "saveFormatPreference", settings.saveFormatPreference },
        { "autoAddStoreModeExtForMalodyExport",
          settings.autoAddStoreModeExtForMalodyExport },
        { "timeFormatPreference", settings.timeFormatPreference },
        { "lastFilePickerPath", settings.lastFilePickerPath },
        { "disableScrollAccelerationWhileDrawing",
          settings.disableScrollAccelerationWhileDrawing },
        { "removeObjectsOnPolylinePath", settings.removeObjectsOnPolylinePath },
        { "enablePolylineEditing", settings.enablePolylineEditing },
        { "enableBmsEditing", settings.enableBmsEditing },
        { "selectPastedObjects", settings.selectPastedObjects },
        { "copyPasteTimeBasis", settings.copyPasteTimeBasis },
        { "timelineSelectionIncludesBpm",
          settings.timelineSelectionIncludesBpm },
        { "bpmMeasurementToolPreferences",
          settings.bpmMeasurementToolPreferences },
        { "softwareCursorConfig", settings.softwareCursorConfig },
        { "preferredAsciiFont", settings.preferredAsciiFont },
        { "preferredCjkFont", settings.preferredCjkFont },
        { "stopPlaybackOnScroll", settings.stopPlaybackOnScroll },
        { "snapFloor", settings.snapFloor },
        { "showTimelineWindow", settings.showTimelineWindow },
        { "timelineProfessionalMode", settings.timelineProfessionalMode },
        { "showPreviewWindow", settings.showPreviewWindow },
        { "showToolLabels", settings.showToolLabels },
        { "fixedToolWindow", settings.fixedToolWindow },
        { "showManagerLabels", settings.showManagerLabels },
        { "aesthetics", settings.aesthetics },
        { "colorPalettes", settings.colorPalettes },
        { "defaultColorPaletteSchemeName",
          settings.defaultColorPaletteSchemeName },
        { "shortcutConfig", settings.shortcutConfig }
    };
}

void from_json(const nlohmann::json& json, EditorSettings& settings)
{
    settings.syncConfig = json.value("syncConfig", SyncConfig());
    settings.sfxConfig  = json.value("sfxConfig", SfxConfig());
    settings.filePickerStyle =
        json.value("filePickerStyle", FilePickerStyle::Native);
    settings.cursorStyle = json.value("cursorStyle", CursorStyle::Software);
    if ( auto themeIterator = json.find("theme");
         themeIterator != json.end() && themeIterator->is_string() ) {
        settings.theme = themeIterator->get<std::string>();
        if ( settings.theme.empty() ) {
            settings.theme = UI_THEME_AUTO_ID;
        } else if ( settings.theme == "MmmDefault" ) {
            settings.theme = "Cecilia";
        }
    } else {
        settings.theme = UI_THEME_AUTO_ID;
    }
    settings.disabledPluginIds =
        json.value("disabledPluginIds", std::vector<std::string>());
    settings.selectedSkinDirectory =
        json.value("selectedSkinDirectory", std::string("mmm-default"));
    settings.beatDivisor         = json.value("beatDivisor", 4);
    settings.overlapTimeWindowMs = json.value("overlapTimeWindowMs", 5.0f);
    settings.reverseScroll       = json.value("reverseScroll", false);
    settings.scrollSnap          = json.value("scrollSnap", false);
    settings.objectPlacementSnap =
        json.value("objectPlacementSnap", settings.scrollSnap);
    settings.objectPlacementSnapMode = json.value(
        "objectPlacementSnapMode", ObjectPlacementSnapMode::CurrentBeatDivisor);
    settings.commonBeatDivisorMask =
        json.value("commonBeatDivisorMask", COMMON_BEAT_DIVISOR_MASK_DEFAULT) &
        COMMON_BEAT_DIVISOR_MASK_ALL;
    settings.recentProjectsLimit = json.value("recentProjectsLimit", 10);
    settings.language            = json.value("language", std::string("zh_cn"));
    settings.defaultCreator =
        normalizeCreatorIdentity(json.value("defaultCreator", std::string()));
    settings.frameLimit = json.value(
        "frameLimit",
        json.contains("vsync")
            ? (json.value("vsync", false) ? FrameLimitPreference::VSync
                                          : FrameLimitPreference::Unlimited)
            : FrameLimitPreference::Refresh2x);
    settings.audioPlaybackBackend =
        json.value("audioPlaybackBackend", AudioPlaybackBackend::SDL);
    settings.sdlAudioOutputDeviceName =
        json.value("sdlAudioOutputDeviceName", std::string());
    settings.openALAudioOutputDeviceName =
        json.value("openALAudioOutputDeviceName", std::string());
    settings.openALSpatialConfig =
        json.value("openALSpatialConfig", OpenALSpatialConfig());
    settings.renderProfileLogging  = json.value("renderProfileLogging", false);
    settings.autoUploadPgoProfiles = json.value("autoUploadPgoProfiles", false);
    settings.pgoProfileUploadConsentAsked = json.value(
        "pgoProfileUploadConsentAsked", json.contains("autoUploadPgoProfiles"));
    settings.fontSizeMultiplier    = json.value("fontSizeMultiplier", 1.15f);
    settings.uiScaleMultiplier     = json.value("uiScaleMultiplier", 1.0f);
    settings.scrollSpeedMultiplier = json.value("scrollSpeedMultiplier", 4.0f);
    settings.globalVolume          = json.value("globalVolume", 0.25f);
    settings.globalMuted           = json.value("globalMuted", false);
    settings.bgmGain               = json.value("bgmGain", 1.0f);
    settings.bgmGainMuted          = json.value("bgmGainMuted", false);
    settings.sfxGain               = json.value("sfxGain", 1.0f);
    settings.sfxGainMuted          = json.value("sfxGainMuted", false);
    settings.interactionSfxGain    = json.value("interactionSfxGain", 1.0f);
    settings.interactionSfxGainMuted =
        json.value("interactionSfxGainMuted", false);
    settings.selectionMode =
        json.value("selectionMode", SelectionMode::Intersection);
    settings.marqueeThickness = json.value("marqueeThickness", 2.0f);
    settings.marqueeRounding  = json.value("marqueeRounding", 0.0f);
    settings.saveFormatPreference =
        json.value("saveFormatPreference", SaveFormatPreference::ForceMMM);
    settings.autoAddStoreModeExtForMalodyExport =
        json.value("autoAddStoreModeExtForMalodyExport", false);
    settings.timeFormatPreference =
        json.value("timeFormatPreference", TimeFormatPreference::Seconds);
    settings.lastFilePickerPath =
        json.value("lastFilePickerPath", std::string("."));
    settings.disableScrollAccelerationWhileDrawing =
        json.value("disableScrollAccelerationWhileDrawing", true);
    settings.removeObjectsOnPolylinePath =
        json.value("removeObjectsOnPolylinePath", false);
    settings.enablePolylineEditing = json.value("enablePolylineEditing", true);
    settings.enableBmsEditing      = json.value("enableBmsEditing", true);
    settings.selectPastedObjects   = json.value("selectPastedObjects", false);
    settings.copyPasteTimeBasis =
        json.value("copyPasteTimeBasis", CopyPasteTimeBasis::Timestamp);
    settings.timelineSelectionIncludesBpm =
        json.value("timelineSelectionIncludesBpm", false);
    BpmMeasurementToolPreferences bpmMeasurementToolPreferencesFallback;
    bpmMeasurementToolPreferencesFallback.beatDivisor = settings.beatDivisor;
    settings.bpmMeasurementToolPreferences            = json.value(
        "bpmMeasurementToolPreferences", bpmMeasurementToolPreferencesFallback);
    settings.softwareCursorConfig =
        json.value("softwareCursorConfig", SoftwareCursorConfig());
    settings.preferredAsciiFont =
        json.value("preferredAsciiFont", std::string("Default"));
    settings.preferredCjkFont =
        json.value("preferredCjkFont", std::string("Default"));
    settings.stopPlaybackOnScroll = json.value("stopPlaybackOnScroll", false);
    settings.snapFloor            = json.value("snapFloor", false);
    settings.showTimelineWindow   = json.value("showTimelineWindow", true);
    settings.timelineProfessionalMode =
        json.value("timelineProfessionalMode", false);
    settings.showPreviewWindow = json.value("showPreviewWindow", true);
    settings.showToolLabels    = json.value("showToolLabels", false);
    settings.fixedToolWindow   = json.value("fixedToolWindow", true);
    settings.showManagerLabels = json.value("showManagerLabels", true);
    settings.aesthetics        = json.value("aesthetics", UIAestheticsConfig());
    settings.colorPalettes = json.value("colorPalettes", ColorPaletteConfig());
    settings.defaultColorPaletteSchemeName =
        json.value("defaultColorPaletteSchemeName",
                   std::string(COLOR_PALETTE_SKIN_DEFAULT_SCHEME_ID));
    settings.shortcutConfig = json.value("shortcutConfig", ShortcutConfig());
}

void to_json(nlohmann::json& json, const EditorConfig& config)
{
    json = nlohmann::json{ { "visual", config.visual },
                           { "settings", config.settings },
                           { "recentProjects", config.recentProjects } };
}

void from_json(const nlohmann::json& json, EditorConfig& config)
{
    config.visual   = json.value("visual", VisualConfig());
    config.settings = json.value("settings", EditorSettings());
    config.recentProjects =
        json.value("recentProjects", std::vector<std::string>());
}

}  // namespace MMM::Config
