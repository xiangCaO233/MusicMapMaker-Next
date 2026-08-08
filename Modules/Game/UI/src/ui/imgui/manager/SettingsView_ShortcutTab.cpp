#include "ui/imgui/manager/SettingsView.h"

#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/utils/UIWidgetUtils.h"

#include "imgui.h"

#include <algorithm>
#include <string>

namespace MMM::UI
{

/// @brief 绘制单个快捷键录制控件。
/// @param binding 正在编辑的快捷键绑定。
/// @param target 当前控件对应的录制目标。
/// @param id ImGui 控件 ID 后缀。
/// @param width 控件可用宽度。
/// @param changed 发生修改时写入 true。
/// @warning UI 热路径：设置窗口打开且当前页为快捷键页时每帧执行；
/// 只查询当前帧键盘状态并更新配置。
void SettingsView::drawShortcutBindingControl(Config::ShortcutBinding& binding,
                                              ShortcutRecordTarget     target,
                                              const char* id, float width,
                                              bool& changed)
{
    ImGui::PushID(id);

    const bool isRecording = m_recordingShortcutTarget == target;
    if ( isRecording ) {
        ShortcutUtils::setShortcutRecordingActive(true);
    }
    if ( isRecording && ImGui::IsKeyPressed(ImGuiKey_Escape, false) ) {
        m_recordingShortcutTarget = ShortcutRecordTarget::None;
        ShortcutUtils::setShortcutRecordingActive(false);
    } else if ( isRecording ) {
        auto captured = ShortcutUtils::capturePressedShortcut();
        if ( captured ) {
            binding                   = *captured;
            changed                   = true;
            m_recordingShortcutTarget = ShortcutRecordTarget::None;
            ShortcutUtils::setShortcutRecordingActive(false);
        }
    }

    const char* recordLabel = TR_CACHE("ui.settings.shortcut.record").data();
    const char* clearLabel  = TR_CACHE("ui.settings.shortcut.clear").data();
    const float spacing     = ImGui::GetStyle().ItemSpacing.x;
    const float recordW     = ImGui::CalcTextSize(recordLabel).x +
                              ImGui::GetStyle().FramePadding.x * 2.0f;
    const float clearW      = ImGui::CalcTextSize(clearLabel).x +
                              ImGui::GetStyle().FramePadding.x * 2.0f;
    const float displayW =
        std::max(80.0f, width - recordW - clearW - spacing * 2.0f);

    std::string displayText;
    if ( m_recordingShortcutTarget == target ) {
        displayText = TR_CACHE("ui.settings.shortcut.recording").data();
    } else {
        displayText = ShortcutUtils::formatShortcut(binding);
        if ( displayText.empty() ) {
            displayText = TR_CACHE("ui.settings.shortcut.none").data();
        }
    }

    std::string displayButtonId = displayText + "###ShortcutDisplay_" + id;
    std::string recordButtonId =
        std::string(recordLabel) + "###ShortcutRecord_" + id;
    std::string clearButtonId =
        std::string(clearLabel) + "###ShortcutClear_" + id;

    if ( ::MMM::UI::FeedbackButton(displayButtonId.c_str(),
                                   ImVec2(displayW, 0.0f)) ) {
        m_recordingShortcutTarget = target;
        ShortcutUtils::setShortcutRecordingActive(true);
    }
    ImGui::SameLine();
    if ( ::MMM::UI::FeedbackButton(recordButtonId.c_str(),
                                   ImVec2(recordW, 0.0f)) ) {
        m_recordingShortcutTarget = target;
        ShortcutUtils::setShortcutRecordingActive(true);
    }
    ImGui::SameLine();
    if ( ::MMM::UI::FeedbackButton(clearButtonId.c_str(),
                                   ImVec2(clearW, 0.0f)) ) {
        binding.enabled = false;
        binding.key.clear();
        changed = true;
        if ( m_recordingShortcutTarget == target ) {
            m_recordingShortcutTarget = ShortcutRecordTarget::None;
            ShortcutUtils::setShortcutRecordingActive(false);
        }
    }

    ImGui::PopID();
}

/// @brief 绘制快捷键设置页。
/// @warning UI 热路径：设置窗口打开且当前页为快捷键页时每帧执行；
/// 只查询当前帧键盘状态并更新配置。
void SettingsView::drawShortcutSettings()
{
    auto& appConfig = Config::AppConfig::instance();
    auto& settings  = appConfig.getEditorSettings();
    bool  changed   = false;

    if ( m_recordingShortcutTarget == ShortcutRecordTarget::None ) {
        ShortcutUtils::setShortcutRecordingActive(false);
    }

    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    const float maxLabelW =
        getCurrentTabLabelWidth(appConfig.getWindowContentScale());

    auto& shortcutSection = getSection(sectionIndex++);
    shortcutSection.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
    m_contentVBox.addLayout(
        "ShortcutSection", shortcutSection, Sizing::Grow(), Sizing::Fit());

    auto addShortcutRow = [&](CLayVBox&                sec,
                              const char*              label,
                              Config::ShortcutBinding& binding,
                              ShortcutRecordTarget     target,
                              const char*              id) {
        Config::ShortcutBinding* bindingPtr  = &binding;
        ShortcutRecordTarget     targetValue = target;
        std::string              idValue     = id ? id : "";
        addSettingItem(sec,
                       rowIndex,
                       label,
                       maxLabelW,
                       [this, bindingPtr, targetValue, idValue, &changed](
                           Clay_BoundingBox r, bool) {
                           drawShortcutBindingControl(*bindingPtr,
                                                      targetValue,
                                                      idValue.c_str(),
                                                      r.width,
                                                      changed);
                       });
    };

    auto& shortcutConfig = settings.shortcutConfig;

    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.tool_move").data(),
                   shortcutConfig.toolMove,
                   ShortcutRecordTarget::ToolMove,
                   "ToolMove");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.tool_marquee").data(),
                   shortcutConfig.toolMarquee,
                   ShortcutRecordTarget::ToolMarquee,
                   "ToolMarquee");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.tool_draw").data(),
                   shortcutConfig.toolDraw,
                   ShortcutRecordTarget::ToolDraw,
                   "ToolDraw");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.tool_color_brush").data(),
                   shortcutConfig.toolColorBrush,
                   ShortcutRecordTarget::ToolColorBrush,
                   "ToolColorBrush");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.tool_color_eraser").data(),
                   shortcutConfig.toolColorEraser,
                   ShortcutRecordTarget::ToolColorEraser,
                   "ToolColorEraser");

    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.mirror").data(),
                   shortcutConfig.mirror,
                   ShortcutRecordTarget::Mirror,
                   "Mirror");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.mirror_paste").data(),
                   shortcutConfig.mirrorPaste,
                   ShortcutRecordTarget::MirrorPaste,
                   "MirrorPaste");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.edit_selected_volume").data(),
                   shortcutConfig.editSelectedVolume,
                   ShortcutRecordTarget::EditSelectedVolume,
                   "EditSelectedVolume");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.delete_selected").data(),
                   shortcutConfig.deleteSelected,
                   ShortcutRecordTarget::DeleteSelected,
                   "DeleteSelected");

    addShortcutRow(
        shortcutSection,
        TR_CACHE("ui.settings.shortcut.toggle_reverse_scroll").data(),
        shortcutConfig.toggleReverseScroll,
        ShortcutRecordTarget::ToggleReverseScroll,
        "ToggleReverseScroll");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.toggle_scroll_snap").data(),
                   shortcutConfig.toggleScrollSnap,
                   ShortcutRecordTarget::ToggleScrollSnap,
                   "ToggleScrollSnap");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.toggle_snap_floor").data(),
                   shortcutConfig.toggleSnapFloor,
                   ShortcutRecordTarget::ToggleSnapFloor,
                   "ToggleSnapFloor");
    addShortcutRow(
        shortcutSection,
        TR_CACHE("ui.settings.shortcut.toggle_scroll_timing_mapping").data(),
        shortcutConfig.toggleScrollTimingMapping,
        ShortcutRecordTarget::ToggleScrollTimingMapping,
        "ToggleScrollTimingMapping");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.toggle_beat_lines").data(),
                   shortcutConfig.toggleBeatLines,
                   ShortcutRecordTarget::ToggleBeatLines,
                   "ToggleBeatLines");
    addShortcutRow(
        shortcutSection,
        TR_CACHE("ui.settings.shortcut.toggle_stop_playback_on_scroll").data(),
        shortcutConfig.toggleStopPlaybackOnScroll,
        ShortcutRecordTarget::ToggleStopPlaybackOnScroll,
        "ToggleStopPlaybackOnScroll");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.toggle_hit_sfx").data(),
                   shortcutConfig.toggleHitSfx,
                   ShortcutRecordTarget::ToggleHitSfx,
                   "ToggleHitSfx");
    addShortcutRow(shortcutSection,
                   TR_CACHE("ui.settings.shortcut.toggle_hit_effects").data(),
                   shortcutConfig.toggleHitEffects,
                   ShortcutRecordTarget::ToggleHitEffects,
                   "ToggleHitEffects");
    addShortcutRow(
        shortcutSection,
        TR_CACHE("ui.settings.shortcut.toggle_sync_same_main_audio").data(),
        shortcutConfig.toggleSyncSameMainAudio,
        ShortcutRecordTarget::ToggleSyncSameMainAudio,
        "ToggleSyncSameMainAudio");

    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    if ( changed ) {
        Event::EventBus::instance().publish(Event::LogicCommandEvent(
            Logic::CmdUpdateEditorConfig{ appConfig.getEditorConfig() }));
        appConfig.save();
    }
}

}  // namespace MMM::UI
