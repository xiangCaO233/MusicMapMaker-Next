#include "ui/imgui/menu/items/MainMenuToolbarVisibilityItem.h"

#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "ui/utils/UIWidgetUtils.h"

namespace MMM::UI
{
namespace
{
/// @brief 绘制一个直接切换持久化布尔值的工具栏可见性菜单项。
/// @param translationKey 菜单文本翻译键。
/// @param visible 对应按钮的可见性状态。
/// @return 状态在本帧被用户修改时返回 true。
/// @warning UI 热路径：只调用统一菜单项反馈入口。
bool drawVisibilityToggle(const char* translationKey, bool& visible)
{
    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
    const bool changed = ::MMM::UI::FeedbackMenuItem(
        TR(translationKey).data(), nullptr, &visible, true);
    ImGui::PopItemFlag();
    return changed;
}
}  // namespace

/// @brief 绘制工具栏按钮可见性分组子菜单。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在查看菜单展开时执行，只读写 ImGui 与配置状态。
void MainMenuToolbarVisibilityItem::render(MainMenuContext& context)
{
    (void)context;
    if ( !::MMM::UI::FeedbackBeginMenu(
             TR("ui.view.toolbar_visibility").data()) ) {
        return;
    }

    auto& visibility =
        Config::AppConfig::instance().getEditorSettings().toolbarVisibility;
    bool changed = false;

    ImGui::SeparatorText(TR("ui.view.toolbar_switch_tools").data());
    changed |=
        drawVisibilityToggle("ui.toolbar.move", visibility.stateTools.move);
    changed |= drawVisibilityToggle("ui.toolbar.marquee",
                                    visibility.stateTools.marquee);
    changed |=
        drawVisibilityToggle("ui.toolbar.draw", visibility.stateTools.draw);
    changed |= drawVisibilityToggle("ui.toolbar.color_brush",
                                    visibility.stateTools.colorBrush);
    changed |= drawVisibilityToggle("ui.toolbar.color_eraser",
                                    visibility.stateTools.colorEraser);
    changed |=
        drawVisibilityToggle("ui.toolbar.layout", visibility.stateTools.layout);

    ImGui::SeparatorText(TR("ui.view.toolbar_edit_tools").data());
    auto& buttons = visibility.independentButtons;
    changed |=
        drawVisibilityToggle("ui.toolbar.note_palette", buttons.notePalette);
    changed |= drawVisibilityToggle("ui.toolbar.magnet_tool", buttons.magnet);
    changed |= drawVisibilityToggle("ui.toolbar.scroll_timing_mapping",
                                    buttons.scrollTimingMapping);
    changed |= drawVisibilityToggle("ui.toolbar.draw_beat_lines",
                                    buttons.beatLineDisplay);
    changed |= drawVisibilityToggle("ui.toolbar.key_sound_tool",
                                    buttons.soundEffectTool);
    changed |= drawVisibilityToggle("ui.toolbar.play_pause", buttons.playback);
    changed |= drawVisibilityToggle("ui.toolbar.playback_speed",
                                    buttons.playbackSpeed);
    changed |=
        drawVisibilityToggle("ui.settings.beatmap.tracks", buttons.trackCount);
    changed |=
        drawVisibilityToggle("ui.toolbar.beat_divisor", buttons.beatDivisor);

    if ( changed ) {
        Config::AppConfig::instance().save();
    }
    ::MMM::UI::FeedbackEndMenu();
}

}  // namespace MMM::UI
