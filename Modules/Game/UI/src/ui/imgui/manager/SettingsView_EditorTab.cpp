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
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIThemeUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <filesystem>
#include <nfd.h>

namespace MMM::UI
{

/// @brief 渲染编辑器设置页。
void SettingsView::drawEditorSettings()
{
    auto& settings = Config::AppConfig::instance().getEditorSettings();
    bool  changed  = false;

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
