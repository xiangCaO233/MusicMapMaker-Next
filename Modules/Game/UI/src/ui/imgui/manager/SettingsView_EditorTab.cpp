#include "audio/AudioManager.h"
#include "canvas/TimeFormatUtils.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "graphic/imguivk/VKContext.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <filesystem>
#include <nfd.h>
#include <string>

namespace MMM::UI
{

/// @brief 绘制单个快捷键录制控件。
/// @param binding 正在编辑的快捷键绑定。
/// @param target 当前控件对应的录制目标。
/// @param id ImGui 控件 ID 后缀。
/// @param width 控件可用宽度。
/// @param changed 发生修改时写入 true。
/// @warning UI 热路径：设置窗口打开且当前页为编辑器页时每帧执行；
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

    const char* recordLabel =
        TR_CACHE("ui.settings.editor.shortcuts.record").data();
    const char* clearLabel =
        TR_CACHE("ui.settings.editor.shortcuts.clear").data();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float recordW = ImGui::CalcTextSize(recordLabel).x +
                          ImGui::GetStyle().FramePadding.x * 2.0f;
    const float clearW  = ImGui::CalcTextSize(clearLabel).x +
                          ImGui::GetStyle().FramePadding.x * 2.0f;
    const float displayW =
        std::max(80.0f, width - recordW - clearW - spacing * 2.0f);

    std::string displayText;
    if ( m_recordingShortcutTarget == target ) {
        displayText = TR_CACHE("ui.settings.editor.shortcuts.recording").data();
    } else {
        displayText = ShortcutUtils::formatShortcut(binding);
        if ( displayText.empty() ) {
            displayText = TR_CACHE("ui.settings.editor.shortcuts.none").data();
        }
    }

    std::string displayButtonId = displayText + "###ShortcutDisplay_" + id;
    std::string recordButtonId =
        std::string(recordLabel) + "###ShortcutRecord_" + id;
    std::string clearButtonId =
        std::string(clearLabel) + "###ShortcutClear_" + id;

    if ( ImGui::Button(displayButtonId.c_str(), ImVec2(displayW, 0.0f)) ) {
        m_recordingShortcutTarget = target;
        ShortcutUtils::setShortcutRecordingActive(true);
    }
    ImGui::SameLine();
    if ( ImGui::Button(recordButtonId.c_str(), ImVec2(recordW, 0.0f)) ) {
        m_recordingShortcutTarget = target;
        ShortcutUtils::setShortcutRecordingActive(true);
    }
    ImGui::SameLine();
    if ( ImGui::Button(clearButtonId.c_str(), ImVec2(clearW, 0.0f)) ) {
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

/// @brief 渲染编辑器设置页。
void SettingsView::drawEditorSettings()
{
    auto& settings = Config::AppConfig::instance().getEditorSettings();
    bool  changed  = false;
    if ( m_recordingShortcutTarget == ShortcutRecordTarget::None ) {
        ShortcutUtils::setShortcutRecordingActive(false);
    }

    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    // 使用布局缓存中的统一标签列宽，避免设置页每帧重复测量全部标签。
    const float maxLabelW = getCurrentTabLabelWidth(
        Config::AppConfig::instance().getWindowContentScale());

    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        std::string baseIdStr = "S" + std::to_string(sectionIndex) + "_R" +
                                std::to_string(rowIndex) + "_H_" + label;
        ImGuiID     id        = ImGui::GetID(baseIdStr.c_str());

        bool isOpen =
            ImGui::GetStateStorage()->GetInt(id, defaultOpen ? 1 : 0) != 0;

        auto& row = getRow(rowIndex++);
        row.setPadding(0, 0, 0, 0).setSpacing(0);
        float h = ImGui::GetFrameHeight();

        row.addElement(
            (baseIdStr + "_el").c_str(),
            Sizing::Grow(),
            Sizing::Fixed(h),
            [label, id, defaultOpen](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos({ r.x, r.y });

                ImVec4 bgCol = ImGui::GetStyle().Colors[ImGuiCol_Header];
                ImGui::PushStyleColor(ImGuiCol_Header, bgCol);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                                      { bgCol.x + 0.05f,
                                        bgCol.y + 0.05f,
                                        bgCol.z + 0.05f,
                                        bgCol.w + 0.1f });
                ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                                      { bgCol.x + 0.1f,
                                        bgCol.y + 0.1f,
                                        bgCol.z + 0.1f,
                                        bgCol.w + 0.15f });

                ImGuiWindow* win         = ImGui::GetCurrentWindow();
                float        savedWRMaxX = win->WorkRect.Max.x;
                win->WorkRect.Max.x      = r.x + r.width;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    { 0.0f, 0.0f });

                bool nowOpen = ImGui::TreeNodeEx(
                    (void*)(intptr_t)id,
                    ImGuiTreeNodeFlags_CollapsingHeader |
                        (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0),
                    "%s",
                    label);

                ImGui::PopStyleVar();
                win->WorkRect.Max.x = savedWRMaxX;

                ImGui::GetStateStorage()->SetInt(id, nowOpen ? 1 : 0);
                ImGui::PopStyleColor(3);
            });

        m_contentVBox.addLayout((baseIdStr + "_layout").c_str(),
                                row,
                                Sizing::Grow(),
                                Sizing::Fixed(h));

        if ( isOpen ) {
            auto& sec = getSection(sectionIndex++);
            sec.setDecorated(true).setSpacing(4).setPadding(8, 8, 8, 8);
            m_contentVBox.addLayout((baseIdStr + "_sec").c_str(),
                                    sec,
                                    Sizing::Grow(),
                                    Sizing::Fit());
            return &sec;
        }
        return nullptr;
    };

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.editor.behavior").data(), true) ) {
        // 采用全局统一最大标签宽度 maxLabelW

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.editor.reverse_scroll").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           changed |= ImGui::Checkbox("##ReverseScroll",
                                                      &settings.reverseScroll);
                       });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.editor.scroll_snap").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           changed |= ImGui::Checkbox("##ScrollSnap",
                                                      &settings.scrollSnap);
                       });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.disable_scroll_accel_while_drawing")
                .data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |= ImGui::Checkbox(
                    "##DisableAccel",
                    &settings.disableScrollAccelerationWhileDrawing);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.remove_objects_on_polyline_path")
                .data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |=
                    ImGui::Checkbox("##RemoveObjectsOnPolylinePath",
                                    &settings.removeObjectsOnPolylinePath);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.select_pasted_objects").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |= ImGui::Checkbox("##SelectPastedObjects",
                                           &settings.selectPastedObjects);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.scroll_multiplier").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat("##ScrollMul",
                                              &settings.scrollSpeedMultiplier,
                                              1.0f,
                                              10.0f,
                                              "%.1f");
                if ( ImGui::IsItemHovered() ) {
                    Utils::renderTooltip(
                        TR("ui.settings.editor.scroll_multiplier_tooltip")
                            .data(),
                        Utils::TooltipDir::Right);
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.beat_divisor").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int beatDivisor = settings.beatDivisor;
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::SliderInt("##BeatDivisor", &beatDivisor, 1, 64) ) {
                    settings.beatDivisor = beatDivisor;
                    changed              = true;
                }
                if ( ImGui::IsItemHovered() ) {
                    Utils::renderTooltip(
                        TR("ui.settings.editor.beat_divisor_tooltip").data(),
                        Utils::TooltipDir::Right);
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.overlap_time_window").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::DragFloat("##OverlapTimeWindowMs",
                                            &settings.overlapTimeWindowMs,
                                            0.1f,
                                            0.0f,
                                            100.0f,
                                            "%.1f ms");
                settings.overlapTimeWindowMs =
                    std::max(0.0f, settings.overlapTimeWindowMs);
            });
    }

    bool shortcutSectionOpen = false;
    if ( auto* sec = addHeader(TR_CACHE("ui.settings.editor.shortcuts").data(),
                               true) ) {
        shortcutSectionOpen  = true;
        auto& shortcutConfig = settings.shortcutConfig;
        auto  addShortcutRow = [&](const char*              label,
                                   Config::ShortcutBinding& binding,
                                   ShortcutRecordTarget     target,
                                   const char*              id) {
            Config::ShortcutBinding* bindingPtr  = &binding;
            ShortcutRecordTarget     targetValue = target;
            std::string              idValue     = id ? id : "";
            addSettingItem(*sec,
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

        addShortcutRow(
            TR_CACHE("ui.settings.editor.shortcuts.tool_move").data(),
            shortcutConfig.toolMove,
            ShortcutRecordTarget::ToolMove,
            "ToolMove");
        addShortcutRow(
            TR_CACHE("ui.settings.editor.shortcuts.tool_marquee").data(),
            shortcutConfig.toolMarquee,
            ShortcutRecordTarget::ToolMarquee,
            "ToolMarquee");
        addShortcutRow(
            TR_CACHE("ui.settings.editor.shortcuts.tool_draw").data(),
            shortcutConfig.toolDraw,
            ShortcutRecordTarget::ToolDraw,
            "ToolDraw");
        addShortcutRow(
            TR_CACHE("ui.settings.editor.shortcuts.tool_color_brush").data(),
            shortcutConfig.toolColorBrush,
            ShortcutRecordTarget::ToolColorBrush,
            "ToolColorBrush");
        addShortcutRow(
            TR_CACHE("ui.settings.editor.shortcuts.tool_color_eraser").data(),
            shortcutConfig.toolColorEraser,
            ShortcutRecordTarget::ToolColorEraser,
            "ToolColorEraser");
        addShortcutRow(TR_CACHE("ui.settings.editor.shortcuts.mirror").data(),
                       shortcutConfig.mirror,
                       ShortcutRecordTarget::Mirror,
                       "Mirror");
        addShortcutRow(
            TR_CACHE("ui.settings.editor.shortcuts.mirror_paste").data(),
            shortcutConfig.mirrorPaste,
            ShortcutRecordTarget::MirrorPaste,
            "MirrorPaste");
        addShortcutRow(
            TR_CACHE("ui.settings.editor.shortcuts.toggle_reverse_scroll")
                .data(),
            shortcutConfig.toggleReverseScroll,
            ShortcutRecordTarget::ToggleReverseScroll,
            "ToggleReverseScroll");
        addShortcutRow(
            TR_CACHE("ui.settings.editor.shortcuts.toggle_scroll_snap").data(),
            shortcutConfig.toggleScrollSnap,
            ShortcutRecordTarget::ToggleScrollSnap,
            "ToggleScrollSnap");
        addShortcutRow(
            TR_CACHE("ui.settings.editor.shortcuts.toggle_snap_floor").data(),
            shortcutConfig.toggleSnapFloor,
            ShortcutRecordTarget::ToggleSnapFloor,
            "ToggleSnapFloor");
        addShortcutRow(
            TR_CACHE(
                "ui.settings.editor.shortcuts.toggle_scroll_timing_mapping")
                .data(),
            shortcutConfig.toggleScrollTimingMapping,
            ShortcutRecordTarget::ToggleScrollTimingMapping,
            "ToggleScrollTimingMapping");
        addShortcutRow(
            TR_CACHE("ui.settings.editor.shortcuts.toggle_beat_lines").data(),
            shortcutConfig.toggleBeatLines,
            ShortcutRecordTarget::ToggleBeatLines,
            "ToggleBeatLines");
        addShortcutRow(
            TR_CACHE(
                "ui.settings.editor.shortcuts.toggle_stop_playback_on_scroll")
                .data(),
            shortcutConfig.toggleStopPlaybackOnScroll,
            ShortcutRecordTarget::ToggleStopPlaybackOnScroll,
            "ToggleStopPlaybackOnScroll");
        addShortcutRow(
            TR_CACHE("ui.settings.editor.shortcuts.toggle_hit_sfx").data(),
            shortcutConfig.toggleHitSfx,
            ShortcutRecordTarget::ToggleHitSfx,
            "ToggleHitSfx");
        addShortcutRow(
            TR_CACHE("ui.settings.editor.shortcuts.toggle_hit_effects").data(),
            shortcutConfig.toggleHitEffects,
            ShortcutRecordTarget::ToggleHitEffects,
            "ToggleHitEffects");
        addShortcutRow(
            TR_CACHE("ui.settings.editor.shortcuts.toggle_sync_same_main_audio")
                .data(),
            shortcutConfig.toggleSyncSameMainAudio,
            ShortcutRecordTarget::ToggleSyncSameMainAudio,
            "ToggleSyncSameMainAudio");
    }
    if ( !shortcutSectionOpen &&
         m_recordingShortcutTarget != ShortcutRecordTarget::None ) {
        m_recordingShortcutTarget = ShortcutRecordTarget::None;
        ShortcutUtils::setShortcutRecordingActive(false);
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.editor.selection").data(),
                               true) ) {
        // 采用全局统一最大标签宽度 maxLabelW

        addRadioSetting(
            *sec,
            rowIndex,
            sectionIndex,
            TR_CACHE("ui.settings.editor.selection").data(),
            maxLabelW,
            { { TR_CACHE("ui.settings.editor.selection.strict").data(),
                (int)Config::SelectionMode::Strict },
              { TR_CACHE("ui.settings.editor.selection.intersection").data(),
                (int)Config::SelectionMode::Intersection } },
            (int&)settings.selectionMode,
            changed);
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.selection.thickness").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat("##MarqueeThick",
                                              &settings.marqueeThickness,
                                              1.0f,
                                              10.0f,
                                              "%.1f px");
            });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.editor.selection.rounding").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |=
                               ImGui::SliderFloat("##MarqueeRound",
                                                  &settings.marqueeRounding,
                                                  0.0f,
                                                  20.0f,
                                                  "%.1f px");
                       });
    }

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.editor.sfx").data(), true) ) {
        // 采用全局统一最大标签宽度 maxLabelW

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.sfx_strategy").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int         strategy = (int)settings.sfxConfig.polylineStrategy;
                const char* strategies[] = {
                    TR_CACHE("ui.settings.editor.sfx_strategy.exact").data(),
                    TR_CACHE(
                        "ui.settings.editor.sfx_strategy.internal_as_normal")
                        .data(),
                    TR_CACHE("ui.settings.editor.sfx_strategy.only_tail_exact")
                        .data(),
                    TR_CACHE("ui.settings.editor.sfx_strategy.all_as_normal")
                        .data()
                };
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::Combo("##SfxStrategy",
                                  &strategy,
                                  strategies,
                                  IM_ARRAYSIZE(strategies)) ) {
                    settings.sfxConfig.polylineStrategy =
                        (Config::PolylineSfxStrategy)strategy;
                    changed = true;
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.sfx_flick_scale").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |= ImGui::Checkbox(
                    "##FlickScale",
                    &settings.sfxConfig.enableFlickWidthVolumeScaling);
            });
        if ( settings.sfxConfig.enableFlickWidthVolumeScaling ) {
            addSettingItem(
                *sec,
                rowIndex,
                TR_CACHE("ui.settings.editor.sfx_flick_mul").data(),
                maxLabelW,
                [&](Clay_BoundingBox r, bool) {
                    ImGui::SetNextItemWidth(r.width);
                    changed |= ImGui::SliderFloat(
                        "##FlickMul",
                        &settings.sfxConfig.flickWidthVolumeMultiplier,
                        0.0f,
                        10.0f);
                });
        }
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.editor.sfx_sync_speed").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                bool syncSpeedChanged = ImGui::Checkbox(
                    "##SyncSpeed", &settings.sfxConfig.hitSfxSyncSpeed);
                if ( syncSpeedChanged ) {
                    changed = true;
                    Audio::AudioManager::instance().updateSFXSyncSpeedRouting(
                        settings.sfxConfig.hitSfxSyncSpeed);
                }
            });
    }

    // 统一执行 Clay 布局渲染
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    ImVec2 sz       = m_contentVBox.renderInCurrent(
        startPos, { ImGui::GetContentRegionAvail().x, 0 });
    ImGui::SetCursorScreenPos({ startPos.x, startPos.y + sz.y });

    if ( changed ) {
        Event::EventBus::instance().publish(
            Event::LogicCommandEvent(Logic::CmdUpdateEditorConfig{
                Config::AppConfig::instance().getEditorConfig() }));
        Config::AppConfig::instance().save();
    }
}

}  // namespace MMM::UI
