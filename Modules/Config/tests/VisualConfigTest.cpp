#include "config/EditorConfig.h"

#include "log/colorful-log.h"

#include <cmath>
#include <nlohmann/json.hpp>

namespace
{

/// @brief 使用小容差比较视觉配置中的单精度数值。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-6F;
}

/// @brief 验证当前分拍线显示模式与自动范围能够完整往返。
/// @return 当前格式往返无损时返回 true。
bool testBeatLineDisplayModeRoundTrip()
{
    MMM::Config::VisualConfig source;
    source.beatLineDisplayMode = MMM::Config::BeatLineDisplayMode::NearCursor;
    source.beatLineCursorVisibleRatio = 0.27F;
    source.beatLineCursorFadeRatio    = 0.31F;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    if ( restored.beatLineDisplayMode !=
             MMM::Config::BeatLineDisplayMode::NearCursor ||
         !near(restored.beatLineCursorVisibleRatio, 0.27F) ||
         !near(restored.beatLineCursorFadeRatio, 0.31F) ||
         !encoded.value("drawBeatLines", false) ) {
        XERROR("Beat line display mode did not survive JSON round trip");
        return false;
    }
    return true;
}

/// @brief 验证旧版 drawBeatLines 布尔值能够迁移为三态显示模式。
/// @return 旧版开关的开启与关闭语义均保持时返回 true。
bool testLegacyDrawBeatLinesMigration()
{
    const nlohmann::json hiddenJson{ { "drawBeatLines", false } };
    const nlohmann::json visibleJson{ { "drawBeatLines", true } };
    const auto           hidden  = hiddenJson.get<MMM::Config::VisualConfig>();
    const auto           visible = visibleJson.get<MMM::Config::VisualConfig>();
    if ( hidden.beatLineDisplayMode !=
             MMM::Config::BeatLineDisplayMode::Hidden ||
         visible.beatLineDisplayMode !=
             MMM::Config::BeatLineDisplayMode::Always ) {
        XERROR("Legacy drawBeatLines value was not migrated");
        return false;
    }
    return true;
}

/// @brief 验证自动显示比例在读取配置时被限制到工具栏允许范围。
/// @return 过小和过大的比例均被正确限制时返回 true。
bool testBeatLineAutoRatioClamping()
{
    const nlohmann::json json{
        { "beatLineDisplayMode", "NearCursor" },
        { "beatLineCursorVisibleRatio", 0.0F },
        { "beatLineCursorFadeRatio", 1.0F },
    };
    const auto config = json.get<MMM::Config::VisualConfig>();
    if ( !near(config.beatLineCursorVisibleRatio, 0.05F) ||
         !near(config.beatLineCursorFadeRatio, 0.40F) ) {
        XERROR("Beat line auto display ratios escaped supported bounds");
        return false;
    }
    return true;
}

/// @brief 验证悬浮检视分拍线单侧延伸比例能够持久化并限制到允许范围。
/// @return 当前值往返、旧配置默认值和上下界限制均正确时返回 true。
bool testHoverSubdivisionLineExtensionRatioConfig()
{
    MMM::Config::VisualConfig source;
    source.hoverSubdivisionLineExtensionRatio = 0.75F;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::VisualConfig>();
    const auto belowMinimum =
        nlohmann::json{ { "hoverSubdivisionLineExtensionRatio", -0.4F } }
            .get<MMM::Config::VisualConfig>();
    const auto aboveMaximum =
        nlohmann::json{ { "hoverSubdivisionLineExtensionRatio", 1.8F } }
            .get<MMM::Config::VisualConfig>();
    if ( !near(restored.hoverSubdivisionLineExtensionRatio, 0.75F) ||
         !near(legacy.hoverSubdivisionLineExtensionRatio, 0.5F) ||
         !near(belowMinimum.hoverSubdivisionLineExtensionRatio, 0.0F) ||
         !near(aboveMaximum.hoverSubdivisionLineExtensionRatio, 1.0F) ) {
        XERROR("Hover subdivision line extension ratio was not normalized");
        return false;
    }
    return true;
}

/// @brief 验证预览区默认隐藏分拍线并继续显示 Timing 线。
/// @return 默认构造和缺省 JSON 均使用相同的安全显示状态时返回 true。
bool testPreviewAreaLineDefaults()
{
    const MMM::Config::PreviewAreaConfig defaults;
    const auto                           restored =
        nlohmann::json::object().get<MMM::Config::PreviewAreaConfig>();
    if ( defaults.drawBeatLines || restored.drawBeatLines ||
         !defaults.drawTimingLines || !restored.drawTimingLines ) {
        XERROR("Preview area line defaults were not preserved");
        return false;
    }
    return true;
}

/// @brief 验证玩家物件绑定音效标签能够显式关闭且缺省配置默认开启。
/// @return 当前格式往返关闭且缺省 JSON 保持开启时返回 true。
bool testBoundSampleLabelConfigRoundTrip()
{
    MMM::Config::VisualConfig source;
    source.showBoundSampleLabels = false;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::VisualConfig>();
    if ( encoded.value("showBoundSampleLabels", true) ||
         restored.showBoundSampleLabels || !legacy.showBoundSampleLabels ) {
        XERROR("Bound sample label config did not preserve compatibility");
        return false;
    }
    return true;
}

/// @brief 验证交互拾取包围盒横纵缩放能够持久化并限制到调试界面范围。
/// @return 往返、缺省值和上下界限制均正确时返回 true。
bool testInteractionHitboxScaleConfig()
{
    MMM::Config::VisualConfig source;
    source.interactionHitboxScaleX = 2.5F;
    source.interactionHitboxScaleY = 0.75F;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::VisualConfig>();
    const auto clamped = nlohmann::json{ { "interactionHitboxScaleX", 0.0F },
                                         { "interactionHitboxScaleY", 8.0F } }
                             .get<MMM::Config::VisualConfig>();
    if ( !near(restored.interactionHitboxScaleX, 2.5F) ||
         !near(restored.interactionHitboxScaleY, 0.75F) ||
         !near(legacy.interactionHitboxScaleX,
               MMM::Config::VisualConfig::DEFAULT_INTERACTION_HITBOX_SCALE) ||
         !near(legacy.interactionHitboxScaleY,
               MMM::Config::VisualConfig::DEFAULT_INTERACTION_HITBOX_SCALE) ||
         !near(clamped.interactionHitboxScaleX,
               MMM::Config::VisualConfig::MIN_INTERACTION_HITBOX_SCALE) ||
         !near(clamped.interactionHitboxScaleY,
               MMM::Config::VisualConfig::MAX_INTERACTION_HITBOX_SCALE) ) {
        XERROR("Interaction hitbox scales escaped supported bounds");
        return false;
    }
    return true;
}

/// @brief 验证非 Hold 打击特效时长能够持久化并兼容旧配置。
/// @return 当前值往返无损、旧配置使用默认值且越界值被限制时返回 true。
bool testNonHoldHitEffectDurationConfig()
{
    MMM::Config::VisualConfig source;
    source.nonHoldHitEffectDuration = 0.48F;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::VisualConfig>();
    const auto tooShort = nlohmann::json{ { "nonHoldHitEffectDuration", 0.0F } }
                              .get<MMM::Config::VisualConfig>();
    const auto tooLong  = nlohmann::json{ { "nonHoldHitEffectDuration", 8.0F } }
                              .get<MMM::Config::VisualConfig>();
    if ( !near(restored.nonHoldHitEffectDuration, 0.48F) ||
         !near(
             legacy.nonHoldHitEffectDuration,
             MMM::Config::VisualConfig::DEFAULT_NON_HOLD_HIT_EFFECT_DURATION) ||
         !near(tooShort.nonHoldHitEffectDuration,
               MMM::Config::VisualConfig::MIN_NON_HOLD_HIT_EFFECT_DURATION) ||
         !near(tooLong.nonHoldHitEffectDuration,
               MMM::Config::VisualConfig::MAX_NON_HOLD_HIT_EFFECT_DURATION) ) {
        XERROR("Non-Hold hit effect duration did not preserve safe bounds");
        return false;
    }
    return true;
}

/// @brief 验证折线编辑开关可持久化且旧配置保持现有完整编辑行为。
/// @return 关闭状态往返不变且缺失字段默认开启时返回 true。
bool testPolylineEditingConfigRoundTrip()
{
    MMM::Config::EditorSettings source;
    source.enablePolylineEditing = false;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    if ( encoded.value("enablePolylineEditing", true) ||
         restored.enablePolylineEditing || !legacy.enablePolylineEditing ) {
        XERROR("Polyline editing config did not preserve compatibility");
        return false;
    }
    return true;
}

/// @brief 验证 BMS 编辑开关可持久化且旧配置默认显示 BGM 轨道。
/// @return 关闭状态往返不变且缺失字段默认开启时返回 true。
bool testBmsEditingConfigRoundTrip()
{
    MMM::Config::EditorSettings source;
    source.enableBmsEditing = false;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    if ( encoded.value("enableBmsEditing", true) || restored.enableBmsEditing ||
         !legacy.enableBmsEditing ) {
        XERROR("BMS editing config did not preserve compatibility");
        return false;
    }
    return true;
}

/// @brief 验证禁止垂直移动设置可持久化且旧配置保持自由拖动。
/// @return 开启状态往返不变且缺失字段默认关闭时返回 true。
bool testVerticalObjectDragConfigRoundTrip()
{
    MMM::Config::EditorSettings source;
    source.disableVerticalObjectDrag = true;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    if ( !encoded.value("disableVerticalObjectDrag", false) ||
         !restored.disableVerticalObjectDrag ||
         legacy.disableVerticalObjectDrag ) {
        XERROR("Vertical object drag config did not preserve compatibility");
        return false;
    }
    return true;
}

/// @brief 验证批量音量编辑快捷键可持久化且旧配置默认不占用键位。
/// @return 自定义组合键往返无损且缺失字段保持禁用时返回 true。
bool testSelectedVolumeShortcutRoundTrip()
{
    MMM::Config::EditorSettings source;
    source.shortcutConfig.editSelectedVolume =
        MMM::Config::ShortcutBinding{ true, "U", true, true, false, false };

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto           legacy =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    const auto& binding       = restored.shortcutConfig.editSelectedVolume;
    const auto& legacyBinding = legacy.shortcutConfig.editSelectedVolume;
    if ( !binding.enabled || binding.key != "U" || !binding.ctrl ||
         !binding.shift || binding.alt || binding.super ||
         legacyBinding.enabled || !legacyBinding.key.empty() ) {
        XERROR("Selected volume shortcut did not preserve compatibility");
        return false;
    }
    return true;
}

/// @brief 验证布局菜单的物件与背景复位仅影响各自管理的配置。
/// @return 两组字段恢复应用默认值且背景电平图等无关字段保持不变时返回 true。
bool testRenderingDefaultsReset()
{
    MMM::Config::EditorConfig config;
    config.visual.noteScaleX               = 2.4F;
    config.visual.noteScaleY               = 0.7F;
    config.visual.nonHoldHitEffectDuration = 0.76F;
    config.visual.showBoundSampleLabels    = false;
    config.visual.noteFillMode = MMM::Config::BackgroundFillMode::Center;
    config.settings.defaultColorPaletteSchemeName = "Custom";
    config.visual.background.fillMode =
        MMM::Config::BackgroundFillMode::Stretch;
    config.visual.background.opaque_ratio            = 0.2F;
    config.visual.background.darken_ratio            = 0.1F;
    config.visual.beatLineAlpha                      = 0.2F;
    config.visual.hoverSubdivisionLineExtensionRatio = 0.9F;
    config.visual.background.spectrum.bandCount      = 64;
    config.visual.background.spectrum.opacity        = 0.8F;

    config.resetNoteRenderingToDefaults();
    const MMM::Config::EditorConfig defaults;
    if ( !near(config.visual.noteScaleX, defaults.visual.noteScaleX) ||
         !near(config.visual.noteScaleY, defaults.visual.noteScaleY) ||
         !near(config.visual.nonHoldHitEffectDuration,
               defaults.visual.nonHoldHitEffectDuration) ||
         config.visual.showBoundSampleLabels !=
             defaults.visual.showBoundSampleLabels ||
         config.visual.noteFillMode != defaults.visual.noteFillMode ||
         config.settings.defaultColorPaletteSchemeName !=
             defaults.settings.defaultColorPaletteSchemeName ||
         config.visual.background.fillMode !=
             MMM::Config::BackgroundFillMode::Stretch ) {
        XERROR("Note rendering reset escaped its configuration boundary");
        return false;
    }

    config.resetBackgroundRenderingToDefaults();
    if ( config.visual.background.fillMode !=
             defaults.visual.background.fillMode ||
         !near(config.visual.background.opaque_ratio,
               defaults.visual.background.opaque_ratio) ||
         !near(config.visual.background.darken_ratio,
               defaults.visual.background.darken_ratio) ||
         !near(config.visual.beatLineAlpha, defaults.visual.beatLineAlpha) ||
         !near(config.visual.hoverSubdivisionLineExtensionRatio,
               defaults.visual.hoverSubdivisionLineExtensionRatio) ||
         config.visual.background.spectrum.bandCount != 64 ||
         !near(config.visual.background.spectrum.opacity, 0.8F) ) {
        XERROR("Background rendering reset escaped its configuration boundary");
        return false;
    }
    return true;
}

/// @brief 验证背景频谱配置能够完整往返。
/// @return 当前格式往返无损时返回 true。
bool testBackgroundSpectrumRoundTrip()
{
    MMM::Config::VisualConfig source;
    auto&                     spectrum = source.background.spectrum;
    auto& placement            = source.canvasComponents.backgroundSpectrum;
    placement.visible          = true;
    placement.anchorX          = 0.37F;
    placement.anchorY          = 0.42F;
    placement.fontSizeRatio    = 0.08F;
    spectrum.bandCount         = 48;
    spectrum.widthRatio        = 0.72F;
    spectrum.heightRatio       = 0.44F;
    spectrum.baselineRatio     = 0.83F;
    spectrum.opacity           = 0.27F;
    spectrum.leftBarColor      = { 0.12F, 0.24F, 0.36F, 0.48F };
    spectrum.rightBarColor     = { 0.51F, 0.62F, 0.73F, 0.84F };
    spectrum.includeHitEffects = true;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    const auto&          result   = restored.background.spectrum;
    const auto& resultPlacement = restored.canvasComponents.backgroundSpectrum;
    if ( !result.enabled || result.bandCount != 48 ||
         !near(result.widthRatio, 0.72F) || !near(result.heightRatio, 0.44F) ||
         !near(result.baselineRatio, 0.83F) || !near(result.opacity, 0.27F) ||
         !near(result.leftBarColor[0], 0.12F) ||
         !near(result.leftBarColor[3], 0.48F) ||
         !near(result.rightBarColor[0], 0.51F) ||
         !near(result.rightBarColor[3], 0.84F) || !result.includeHitEffects ||
         !resultPlacement.visible || !near(resultPlacement.anchorX, 0.37F) ||
         !near(resultPlacement.anchorY, 0.42F) ||
         !near(resultPlacement.fontSizeRatio, 0.08F) ) {
        XERROR("Background spectrum config did not survive JSON round trip");
        return false;
    }
    return true;
}

/// @brief 验证旧版背景频谱显隐和底边位置迁移到画布组件布局。
/// @return 旧字段生成可见组件且保持原始垂直位置时返回 true。
bool testLegacyBackgroundSpectrumMigration()
{
    const nlohmann::json json{
        { "background",
          { { "spectrum",
              { { "enabled", true },
                { "heightRatio", 0.4F },
                { "baselineRatio", 0.9F } } } } },
    };
    const auto  config    = json.get<MMM::Config::VisualConfig>();
    const auto& placement = config.canvasComponents.backgroundSpectrum;
    if ( !placement.visible || !config.background.spectrum.enabled ||
         config.background.spectrum.includeHitEffects ||
         !near(placement.anchorY, 0.7F) ) {
        XERROR(
            "Legacy background spectrum migration did not use safe defaults");
        return false;
    }
    return true;
}

/// @brief 验证背景频谱配置在读取时被限制到设置菜单允许范围。
/// @return 所有越界字段均被正确限制时返回 true。
bool testBackgroundSpectrumClamping()
{
    const nlohmann::json json{
        { "background",
          { { "spectrum",
              { { "bandCount", 2 },
                { "widthRatio", 0.0F },
                { "heightRatio", 2.0F },
                { "baselineRatio", -1.0F },
                { "opacity", 3.0F },
                { "leftBarColor", { -1.0F, 0.25F, 2.0F, 0.75F } },
                { "rightBarColor", { 1.5F, -0.5F, 0.5F, 2.0F } } } } } }
    };
    const auto  config   = json.get<MMM::Config::VisualConfig>();
    const auto& spectrum = config.background.spectrum;
    if ( spectrum.bandCount != MMM::Config::BACKGROUND_SPECTRUM_MIN_BANDS ||
         !near(spectrum.widthRatio, 0.10F) ||
         !near(spectrum.heightRatio, 1.0F) ||
         !near(spectrum.baselineRatio, 0.05F) ||
         !near(spectrum.opacity, 1.0F) ||
         !near(spectrum.leftBarColor[0], 0.0F) ||
         !near(spectrum.leftBarColor[1], 0.25F) ||
         !near(spectrum.leftBarColor[2], 1.0F) ||
         !near(spectrum.leftBarColor[3], 0.75F) ||
         !near(spectrum.rightBarColor[0], 1.0F) ||
         !near(spectrum.rightBarColor[1], 0.0F) ||
         !near(spectrum.rightBarColor[2], 0.5F) ||
         !near(spectrum.rightBarColor[3], 1.0F) ) {
        XERROR("Background spectrum config escaped supported bounds");
        return false;
    }
    return true;
}

/// @brief 验证不同 Key 数的轨道、判定线与组件布局独立保存。
/// @return 独立编辑、旧配置继承和 JSON 往返均正确时返回 true。
bool testKeyCountLayoutIsolationAndMigration()
{
    MMM::Config::VisualConfig source;
    source.trackLayout.left                    = 0.11F;
    source.judgeline_pos                       = 0.81F;
    source.canvasComponents.beatNumber.anchorX = 0.13F;

    auto& fourTrackLayout = source.editableTrackLayoutForKeyCount(4);
    fourTrackLayout.left  = 0.21F;
    fourTrackLayout.right = 0.61F;
    source.editableJudgmentLinePositionForKeyCount(4) = 0.74F;
    auto& fourComponents = source.editableCanvasComponentsForKeyCount(4);
    fourComponents.beatNumber.visible = true;
    fourComponents.beatNumber.anchorX = 0.24F;

    auto& sevenTrackLayout = source.editableTrackLayoutForKeyCount(7);
    sevenTrackLayout.left  = 0.31F;
    sevenTrackLayout.right = 0.91F;
    source.editableJudgmentLinePositionForKeyCount(7) = 0.88F;
    auto& sevenComponents = source.editableCanvasComponentsForKeyCount(7);
    sevenComponents.beatNumber.visible = false;
    sevenComponents.beatNumber.anchorX = 0.67F;

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::VisualConfig>();
    const auto&          restoredFourTrack = restored.trackLayoutForKeyCount(4);
    const auto& restoredSevenTrack         = restored.trackLayoutForKeyCount(7);
    const auto& restoredLegacyTrack        = restored.trackLayoutForKeyCount(5);
    const auto& restoredFourComponents =
        restored.canvasComponentsForKeyCount(4);
    const auto& restoredSevenComponents =
        restored.canvasComponentsForKeyCount(7);
    const auto& restoredLegacyComponents =
        restored.canvasComponentsForKeyCount(5);

    auto materialized = restored;
    materialized.applyKeyCountLayout(7);
    const auto legacy =
        nlohmann::json{
            { "trackLayout",
              { { "left", 0.17F },
                { "top", 0.05F },
                { "right", 0.77F },
                { "bottom", 0.95F } } },
            { "judgeline_pos", 0.79F },
            { "canvasComponents",
              { { "beatNumber", { { "anchorX", 0.29F } } } } },
        }
            .get<MMM::Config::VisualConfig>();

    if ( source.keyCountLayouts.size() != 2U ||
         restored.keyCountLayouts.size() != 2U ||
         !near(restoredFourTrack.left, 0.21F) ||
         !near(restoredFourTrack.right, 0.61F) ||
         !near(restoredSevenTrack.left, 0.31F) ||
         !near(restoredSevenTrack.right, 0.91F) ||
         !near(restoredLegacyTrack.left, 0.11F) ||
         !near(restored.judgmentLinePositionForKeyCount(4), 0.74F) ||
         !near(restored.judgmentLinePositionForKeyCount(7), 0.88F) ||
         !near(restored.judgmentLinePositionForKeyCount(5), 0.81F) ||
         !restoredFourComponents.beatNumber.visible ||
         !near(restoredFourComponents.beatNumber.anchorX, 0.24F) ||
         restoredSevenComponents.beatNumber.visible ||
         !near(restoredSevenComponents.beatNumber.anchorX, 0.67F) ||
         !near(restoredLegacyComponents.beatNumber.anchorX, 0.13F) ||
         !near(materialized.trackLayout.left, 0.31F) ||
         !near(materialized.judgeline_pos, 0.88F) ||
         !near(materialized.canvasComponents.beatNumber.anchorX, 0.67F) ||
         !legacy.keyCountLayouts.empty() ||
         !near(legacy.trackLayoutForKeyCount(4).left, 0.17F) ||
         !near(legacy.judgmentLinePositionForKeyCount(7), 0.79F) ||
         !near(legacy.canvasComponentsForKeyCount(9).beatNumber.anchorX,
               0.29F) ) {
        XERROR("Key-count layouts were shared or legacy migration failed");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行视觉配置兼容性与默认值测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testBeatLineDisplayModeRoundTrip() &&
                   testLegacyDrawBeatLinesMigration() &&
                   testBeatLineAutoRatioClamping() &&
                   testHoverSubdivisionLineExtensionRatioConfig() &&
                   testPreviewAreaLineDefaults() &&
                   testBoundSampleLabelConfigRoundTrip() &&
                   testInteractionHitboxScaleConfig() &&
                   testNonHoldHitEffectDurationConfig() &&
                   testPolylineEditingConfigRoundTrip() &&
                   testBmsEditingConfigRoundTrip() &&
                   testVerticalObjectDragConfigRoundTrip() &&
                   testSelectedVolumeShortcutRoundTrip() &&
                   testRenderingDefaultsReset() &&
                   testBackgroundSpectrumRoundTrip() &&
                   testLegacyBackgroundSpectrumMigration() &&
                   testBackgroundSpectrumClamping() &&
                   testKeyCountLayoutIsolationAndMigration()
               ? 0
               : 1;
}
