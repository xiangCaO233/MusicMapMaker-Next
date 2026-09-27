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
/// @note 禁用弹窗自动关闭，使用户可连续配置多个工具栏按钮。
bool drawVisibilityToggle(const char* translationKey, bool& visible)
{
    // 标志作用域严格包围单个菜单项，避免影响后续菜单控件。
    ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
    const bool changed = ::MMM::UI::FeedbackMenuItem(
        TR(translationKey).data(), nullptr, &visible, true);
    ImGui::PopItemFlag();
    // 将变更结果交给上层合并，统一决定是否持久化配置。
    return changed;
}
}  // namespace

/// @brief 绘制工具栏按钮可见性分组子菜单。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在查看菜单展开时执行，只读写 ImGui 与配置状态。
/// @note 同一帧内聚合全部修改，最多调用一次配置保存。
/// @note 菜单保持展开，便于用户批量调整多个按钮的可见性。
void MainMenuToolbarVisibilityItem::render(MainMenuContext& context)
{
    (void)context;
    // 子菜单未展开时不访问编辑器配置，缩短常规帧路径。
    if ( !::MMM::UI::FeedbackBeginMenu(
             TR("ui.view.toolbar_visibility").data()) ) {
        return;
    }

    // 直接引用持久化配置，所有开关均更新同一份权威状态。
    auto& visibility =
        Config::AppConfig::instance().getEditorSettings().toolbarVisibility;
    bool changed = false;

    // 状态工具组对应互斥或协作的画布操作模式入口。
    ImGui::SeparatorText(TR("ui.view.toolbar_switch_tools").data());
    // 移动、框选与绘制是基础编辑状态工具。
    changed |=
        drawVisibilityToggle("ui.toolbar.move", visibility.stateTools.move);
    changed |= drawVisibilityToggle("ui.toolbar.marquee",
                                    visibility.stateTools.marquee);
    changed |=
        drawVisibilityToggle("ui.toolbar.draw", visibility.stateTools.draw);
    // 颜色笔刷和橡皮擦共享颜色编辑职责，但可独立隐藏。
    changed |= drawVisibilityToggle("ui.toolbar.color_brush",
                                    visibility.stateTools.colorBrush);
    changed |= drawVisibilityToggle("ui.toolbar.color_eraser",
                                    visibility.stateTools.colorEraser);
    // 布局工具单独控制轨道布局编辑模式入口。
    changed |=
        drawVisibilityToggle("ui.toolbar.layout", visibility.stateTools.layout);

    // 独立按钮组不参与状态工具互斥，只控制单项功能入口。
    ImGui::SeparatorText(TR("ui.view.toolbar_edit_tools").data());
    // 使用短引用降低后续成员访问噪声，不改变配置所有权。
    auto& buttons = visibility.independentButtons;
    // 调色板与磁吸工具提供高频编辑辅助入口。
    changed |=
        drawVisibilityToggle("ui.toolbar.note_palette", buttons.notePalette);
    changed |= drawVisibilityToggle("ui.toolbar.magnet_tool", buttons.magnet);
    changed |= drawVisibilityToggle("ui.toolbar.scroll_timing_mapping",
                                    buttons.scrollTimingMapping);
    // 节拍线与按键音分别控制画布辅助显示和试听工具入口。
    changed |= drawVisibilityToggle("ui.toolbar.draw_beat_lines",
                                    buttons.beatLineDisplay);
    changed |= drawVisibilityToggle("ui.toolbar.key_sound_tool",
                                    buttons.soundEffectTool);
    // 播放按钮可独立于速度控件显示，兼容紧凑工具栏布局。
    changed |= drawVisibilityToggle("ui.toolbar.play_pause", buttons.playback);
    // 播放速度与轨道数量属于当前编辑会话的快速参数入口。
    changed |= drawVisibilityToggle("ui.toolbar.playback_speed",
                                    buttons.playbackSpeed);
    changed |=
        drawVisibilityToggle("ui.settings.beatmap.tracks", buttons.trackCount);
    // 节拍细分按钮保留为独立入口，便于只展示节奏编辑控件。
    changed |=
        drawVisibilityToggle("ui.toolbar.beat_divisor", buttons.beatDivisor);

    // 只有实际切换过开关才写配置，避免展开菜单导致无效磁盘写入。
    if ( changed ) {
        Config::AppConfig::instance().save();
    }
    // 始终配对结束子菜单，维持 ImGui 栈平衡。
    ::MMM::UI::FeedbackEndMenu();
}

}  // namespace MMM::UI
