#include "config/EditorSettings.h"
#include "log/colorful-log.h"

#include <nlohmann/json.hpp>

namespace
{

/// @brief 判断状态机工具集合是否全部可见。
/// @param visibility 待检查的状态机工具可见性配置。
/// @return 全部按钮均可见时返回 true。
bool areAllStateToolsVisible(
    const MMM::Config::ToolbarStateToolVisibility& visibility)
{
    return visibility.move && visibility.marquee && visibility.draw &&
           visibility.colorBrush && visibility.colorEraser && visibility.layout;
}

/// @brief 判断独立按钮集合是否全部可见。
/// @param visibility 待检查的独立按钮可见性配置。
/// @return 全部按钮均可见时返回 true。
bool areAllIndependentButtonsVisible(
    const MMM::Config::ToolbarIndependentButtonVisibility& visibility)
{
    return visibility.notePalette && visibility.magnet &&
           visibility.scrollTimingMapping && visibility.beatLineDisplay &&
           visibility.soundEffectTool && visibility.playback &&
           visibility.playbackSpeed && visibility.trackCount &&
           visibility.beatDivisor;
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

/// @brief 验证旧配置缺少工具栏可见性字段时继续显示全部按钮。
/// @return 两个集合中的全部按钮均默认可见时返回 true。
bool testLegacyToolbarVisibilityDefaults()
{
    const auto restored =
        nlohmann::json::object().get<MMM::Config::EditorSettings>();
    if ( !areAllStateToolsVisible(restored.toolbarVisibility.stateTools) ||
         !areAllIndependentButtonsVisible(
             restored.toolbarVisibility.independentButtons) ) {
        XERROR("Legacy toolbar visibility config did not show all buttons");
        return false;
    }
    return true;
}

/// @brief 验证部分配置只覆盖明确写出的按钮，其余按钮保持可见。
/// @return 抓取工具被隐藏且其余按钮保持可见时返回 true。
bool testPartialToolbarVisibilityDefaults()
{
    const nlohmann::json partial{
        { "toolbarVisibility", { { "stateTools", { { "move", false } } } } },
    };
    const auto restored   = partial.get<MMM::Config::EditorSettings>();
    auto       stateTools = restored.toolbarVisibility.stateTools;
    stateTools.move       = true;
    if ( restored.toolbarVisibility.stateTools.move ||
         !areAllStateToolsVisible(stateTools) ||
         !areAllIndependentButtonsVisible(
             restored.toolbarVisibility.independentButtons) ) {
        XERROR("Partial toolbar visibility config changed omitted buttons");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行工具栏按钮可见性持久化与兼容性测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testToolbarVisibilityRoundTrip() &&
                   testLegacyToolbarVisibilityDefaults() &&
                   testPartialToolbarVisibilityDefaults()
               ? 0
               : 1;
}
