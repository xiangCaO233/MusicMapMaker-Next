#include "config/EditorSettings.h"
#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

namespace
{

/// @brief 判断工具栏可见性是否匹配软件默认精简布局。
/// @param visibility 待检查的工具栏可见性配置。
/// @return 与默认显示配置完全一致时返回 true。
bool matchesDefaultToolbarVisibility(
    const MMM::Config::ToolbarVisibilityConfig& visibility)
{
    const auto& tools   = visibility.stateTools;
    const auto& buttons = visibility.independentButtons;
    return tools.move && tools.marquee && tools.draw && !tools.colorBrush &&
           !tools.colorEraser && tools.layout && !buttons.notePalette &&
           buttons.magnet && buttons.scrollTimingMapping &&
           buttons.beatLineDisplay && buttons.soundEffectTool &&
           buttons.playback && !buttons.playbackSpeed && !buttons.trackCount &&
           !buttons.beatDivisor;
}

/// @brief 验证两个工具栏按钮集合的全部隐藏状态可以完整往返。
/// @return 所有字段均写出并恢复为隐藏时返回 true。
bool testToolbarVisibilityRoundTrip()
{
    MMM::Config::EditorSettings source;
    source.toolbarVisibility.stateTools = {
        .move        = false,
        .marquee     = false,
        .draw        = false,
        .colorBrush  = false,
        .colorEraser = false,
        .layout      = false,
    };
    source.toolbarVisibility.independentButtons = {
        .notePalette         = false,
        .magnet              = false,
        .scrollTimingMapping = false,
        .beatLineDisplay     = false,
        .soundEffectTool     = false,
        .playback            = false,
        .playbackSpeed       = false,
        .trackCount          = false,
        .beatDivisor         = false,
    };

    const nlohmann::json encoded  = source;
    const auto           restored = encoded.get<MMM::Config::EditorSettings>();
    const auto&          stateTools = restored.toolbarVisibility.stateTools;
    const auto& buttons = restored.toolbarVisibility.independentButtons;
    if ( stateTools.move || stateTools.marquee || stateTools.draw ||
         stateTools.colorBrush || stateTools.colorEraser || stateTools.layout ||
         buttons.notePalette || buttons.magnet || buttons.scrollTimingMapping ||
         buttons.beatLineDisplay || buttons.soundEffectTool ||
         buttons.playback || buttons.playbackSpeed || buttons.trackCount ||
         buttons.beatDivisor ||
         !encoded.at("toolbarVisibility").contains("stateTools") ||
         !encoded.at("toolbarVisibility").contains("independentButtons") ) {
        XERROR("Toolbar visibility config did not round trip");
        return false;
    }
    return true;
}

/// @brief 验证缺少工具栏可见性字段时使用软件默认精简布局。
/// @return 显示抓取、框选、绘制、布局与常用独立按钮时返回 true。
bool testToolbarVisibilityDefaults()
{
    const auto restored =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    if ( !matchesDefaultToolbarVisibility(restored.toolbarVisibility) ) {
        XERROR("Toolbar visibility config did not use compact defaults");
        return false;
    }
    return true;
}

/// @brief 验证部分配置只覆盖明确写出的按钮，其余按钮保持软件默认值。
/// @return 抓取工具被隐藏且其余按钮仍匹配默认精简布局时返回 true。
bool testPartialToolbarVisibilityDefaults()
{
    const nlohmann::json partial{
        { "toolbarVisibility", { { "stateTools", { { "move", false } } } } },
    };
    auto restored = partial.get<MMM::Config::EditorSettings>();
    if ( restored.toolbarVisibility.stateTools.move ) {
        XERROR("Partial toolbar visibility config did not hide move tool");
        return false;
    }
    restored.toolbarVisibility.stateTools.move = true;
    if ( !matchesDefaultToolbarVisibility(restored.toolbarVisibility) ) {
        XERROR("Partial toolbar visibility config changed omitted defaults");
        return false;
    }
    return true;
}

/// @brief 验证项目配置合并时保留全软件工具栏显示设置。
/// @return 标签、固定模式和全部按钮可见性均取自全局设置时返回 true。
bool testGlobalToolbarVisibilityPreservation()
{
    MMM::Config::EditorSettings globalSettings;
    globalSettings.showToolLabels                                  = true;
    globalSettings.fixedToolWindow                                 = false;
    globalSettings.showManagerLabels                               = false;
    globalSettings.toolbarVisibility.stateTools.colorBrush         = true;
    globalSettings.toolbarVisibility.independentButtons.trackCount = true;

    MMM::Config::EditorSettings projectSettings;
    projectSettings.showToolLabels                                  = false;
    projectSettings.fixedToolWindow                                 = true;
    projectSettings.showManagerLabels                               = true;
    projectSettings.toolbarVisibility.stateTools.colorBrush         = false;
    projectSettings.toolbarVisibility.independentButtons.trackCount = false;

    MMM::Config::preserveGlobalToolbarDisplaySettings(projectSettings,
                                                      globalSettings);
    return projectSettings.showToolLabels && !projectSettings.fixedToolWindow &&
           !projectSettings.showManagerLabels &&
           projectSettings.toolbarVisibility.stateTools.colorBrush &&
           projectSettings.toolbarVisibility.independentButtons.trackCount;
}

}  // namespace

/// @brief 运行工具栏按钮可见性持久化与兼容性测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testToolbarVisibilityRoundTrip() &&
                   testToolbarVisibilityDefaults() &&
                   testPartialToolbarVisibilityDefaults() &&
                   testGlobalToolbarVisibilityPreservation()
               ? 0
               : 1;
}
