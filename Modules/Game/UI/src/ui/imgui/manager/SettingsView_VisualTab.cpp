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
#include <filesystem>
#include <nfd.h>

namespace MMM::UI
{

/// @brief 渲染视觉设置页。
void SettingsView::drawVisualSettings()
{
    auto& visual  = Config::AppConfig::instance().getVisualConfig();
    bool  changed = false;

    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    // 收集本面板所有 Segments 的全部标签，计算统一的全局最大标签宽度，确保跨
    // Seg 完美垂直对齐！
    const char* allVisualLabels[] = {
        TR_CACHE("ui.settings.visual.layout_left").data(),
        TR_CACHE("ui.settings.visual.layout_top").data(),
        TR_CACHE("ui.settings.visual.layout_right").data(),
        TR_CACHE("ui.settings.visual.layout_bottom").data(),
        TR_CACHE("ui.settings.visual.layout_box_width").data(),
        TR_CACHE("ui.settings.visual.judgeline_pos").data(),
        TR_CACHE("ui.settings.visual.beat_line_alpha").data(),
        TR_CACHE("ui.settings.visual.beat_line_before_first_timing").data(),
        TR_CACHE("ui.settings.visual.note_scale_x").data(),
        TR_CACHE("ui.settings.visual.note_scale_y").data(),
        TR_CACHE("ui.settings.visual.note_fill_mode").data(),
        TR_CACHE("ui.settings.visual.debug_draw_hitboxes").data(),
        TR_CACHE("ui.settings.visual.bg_fill_mode").data(),
        TR_CACHE("ui.settings.visual.bg_opaque").data(),
        TR_CACHE("ui.settings.visual.bg_darken").data(),
        TR_CACHE("ui.settings.visual.preview_ratio").data(),
        TR_CACHE("ui.settings.visual.preview_edge_scroll_sensitivity").data(),
        TR_CACHE("ui.settings.visual.preview_margin_left").data(),
        TR_CACHE("ui.settings.visual.preview_margin_top").data(),
        TR_CACHE("ui.settings.visual.preview_margin_right").data(),
        TR_CACHE("ui.settings.visual.preview_margin_bottom").data(),
        TR_CACHE("ui.settings.visual.preview_draw_beat_lines").data(),
        TR_CACHE("ui.settings.visual.preview_draw_timing_lines").data(),
        TR_CACHE("ui.settings.visual.timeline_zoom").data(),
        TR_CACHE("ui.settings.visual.linear_scroll").data(),
        TR_CACHE("ui.settings.visual.snap_threshold").data(),
        TR_CACHE("ui.settings.visual.visual_offset").data()
    };
    float maxLabelW = 0;
    for ( auto* l : allVisualLabels ) {
        maxLabelW = std::max(maxLabelW, measureLabelWidth(l));
    }
    maxLabelW += 16.0f;  // 留出充足间距，确保高 DPI 和多语言下排版美观

    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        std::string baseIdStr = "VS_S" + std::to_string(sectionIndex) + "_R" +
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
             addHeader(TR_CACHE("ui.settings.visual.layout").data(), true) ) {
        // 采用全局统一最大标签宽度 maxLabelW

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.layout_left").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           if ( ImGui::SliderFloat("##LayoutLeft",
                                                   &visual.trackLayout.left,
                                                   0.0f,
                                                   1.0f) ) {
                               visual.trackLayout.left =
                                   std::min(visual.trackLayout.left,
                                            visual.trackLayout.right - 0.01f);
                               changed = true;
                           }
                       });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.layout_top").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::SliderFloat(
                         "##LayoutTop", &visual.trackLayout.top, 0.0f, 1.0f) ) {
                    visual.trackLayout.top =
                        std::min(visual.trackLayout.top,
                                 visual.trackLayout.bottom - 0.01f);
                    changed = true;
                }
            });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.layout_right").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           if ( ImGui::SliderFloat("##LayoutRight",
                                                   &visual.trackLayout.right,
                                                   0.0f,
                                                   1.0f) ) {
                               visual.trackLayout.right =
                                   std::max(visual.trackLayout.right,
                                            visual.trackLayout.left + 0.01f);
                               changed = true;
                           }
                       });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.layout_bottom").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           if ( ImGui::SliderFloat("##LayoutBottom",
                                                   &visual.trackLayout.bottom,
                                                   0.0f,
                                                   1.0f) ) {
                               visual.trackLayout.bottom =
                                   std::max(visual.trackLayout.bottom,
                                            visual.trackLayout.top + 0.01f);
                               changed = true;
                           }
                       });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.layout_box_width").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##LayoutBoxWidth", &visual.trackBoxLineWidth, 1.0f, 10.0f);
            });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.judgeline").data(),
                               true) ) {
        // 采用全局统一最大标签宽度 maxLabelW
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.judgeline_pos").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##JudgeLinePos", &visual.judgeline_pos, 0.0f, 1.0f);
            });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.beat_line").data(),
                               true) ) {
        // 采用全局统一最大标签宽度 maxLabelW
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.beat_line_alpha").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##BeatLineAlpha", &visual.beatLineAlpha, 0.0f, 1.0f);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.beat_line_before_first_timing").data(),
            maxLabelW,
            [&](Clay_BoundingBox, bool) {
                changed |=
                    ImGui::Checkbox("##BeatLineBeforeFirstTiming",
                                    &visual.drawBeatLinesBeforeFirstTiming);
            });
    }

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.visual.note").data(), true) ) {
        // 采用全局统一最大标签宽度 maxLabelW

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.note_scale_x").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ImGui::SliderFloat(
                               "##NoteScaleX", &visual.noteScaleX, 0.5f, 3.0f);
                       });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.note_scale_y").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ImGui::SliderFloat(
                               "##NoteScaleY", &visual.noteScaleY, 0.5f, 3.0f);
                       });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.note_fill_mode").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int         noteFillMode = (int)visual.noteFillMode;
                const char* fillModes[]  = {
                    TR_CACHE("ui.settings.visual.fill_mode.stretch").data(),
                    TR_CACHE("ui.settings.visual.fill_mode.aspect_fit").data(),
                    TR_CACHE("ui.settings.visual.fill_mode.aspect_fill").data(),
                    TR_CACHE("ui.settings.visual.fill_mode.center").data()
                };
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::Combo("##NoteFillMode",
                                  &noteFillMode,
                                  fillModes,
                                  IM_ARRAYSIZE(fillModes)) ) {
                    visual.noteFillMode =
                        (Config::BackgroundFillMode)noteFillMode;
                    changed = true;
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.debug_draw_hitboxes").data(),
            maxLabelW,
            [&](Clay_BoundingBox, bool) {
                changed |= ImGui::Checkbox("##DebugDrawHitboxes",
                                           &visual.debugDrawHitboxes);
            });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.background").data(),
                               true) ) {
        // 采用全局统一最大标签宽度 maxLabelW

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.bg_fill_mode").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                int         bgFillMode  = (int)visual.background.fillMode;
                const char* fillModes[] = {
                    TR_CACHE("ui.settings.visual.fill_mode.stretch").data(),
                    TR_CACHE("ui.settings.visual.fill_mode.aspect_fit").data(),
                    TR_CACHE("ui.settings.visual.fill_mode.aspect_fill").data(),
                    TR_CACHE("ui.settings.visual.fill_mode.center").data()
                };
                ImGui::SetNextItemWidth(r.width);
                if ( ImGui::Combo("##BgFillMode",
                                  &bgFillMode,
                                  fillModes,
                                  IM_ARRAYSIZE(fillModes)) ) {
                    visual.background.fillMode =
                        (Config::BackgroundFillMode)bgFillMode;
                    changed = true;
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.bg_opaque").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##BgOpaque", &visual.background.opaque_ratio, 0.0f, 1.0f);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.bg_darken").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##BgDarken", &visual.background.darken_ratio, 0.0f, 1.0f);
            });
    }

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.visual.preview").data(), true) ) {
        const char* labels[] = {
            TR_CACHE("ui.settings.visual.preview_ratio").data(),
            TR_CACHE("ui.settings.visual.preview_edge_scroll_sensitivity")
                .data(),
            TR_CACHE("ui.settings.visual.preview_margin_left").data(),
            TR_CACHE("ui.settings.visual.preview_margin_top").data(),
            TR_CACHE("ui.settings.visual.preview_margin_right").data(),
            TR_CACHE("ui.settings.visual.preview_margin_bottom").data(),
            TR_CACHE("ui.settings.visual.preview_draw_beat_lines").data(),
            TR_CACHE("ui.settings.visual.preview_draw_timing_lines").data(),
            TR_CACHE("ui.settings.visual.timeline_zoom").data(),
            TR_CACHE("ui.settings.visual.linear_scroll").data(),
            TR_CACHE("ui.settings.visual.snap_threshold").data(),
        };
        // 采用全局统一最大标签宽度 maxLabelW

        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.preview_ratio").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ImGui::SliderFloat(
                               "##PreviewRatio",
                               &visual.previewConfig.areaRatio,
                               1.0f,
                               10.0f);
                       });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_edge_scroll_sensitivity")
                .data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##EdgeSens",
                    &visual.previewConfig.edgeScrollSensitivity,
                    0.0f,
                    5.0f,
                    "%.2f");
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_left").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat("##MarginL",
                                              &visual.previewConfig.margin.left,
                                              0.0f,
                                              20.0f);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_top").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat(
                    "##MarginT", &visual.previewConfig.margin.top, 0.0f, 20.0f);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_right").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |=
                    ImGui::SliderFloat("##MarginR",
                                       &visual.previewConfig.margin.right,
                                       0.0f,
                                       20.0f);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_bottom").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |=
                    ImGui::SliderFloat("##MarginB",
                                       &visual.previewConfig.margin.bottom,
                                       0.0f,
                                       20.0f);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_draw_beat_lines").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |= ImGui::Checkbox("##DrawBeatLines",
                                           &visual.previewConfig.drawBeatLines);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_draw_timing_lines").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |= ImGui::Checkbox(
                    "##DrawTimingLines", &visual.previewConfig.drawTimingLines);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.timeline_zoom").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ImGui::SliderFloat("##TimelineZoom",
                                              &visual.timelineZoom,
                                              0.1f,
                                              5.0f,
                                              "%.2fx");
                if ( ImGui::IsItemHovered() ) {
                    Utils::renderTooltip(
                        TR("ui.settings.visual.timeline_zoom_tooltip").data(),
                        Utils::TooltipDir::Right);
                }
            });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.linear_scroll").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           changed |= ImGui::Checkbox(
                               "##LinearScroll",
                               &visual.enableLinearScrollMapping);
                       });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.snap_threshold").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ImGui::SliderFloat("##SnapThreshold",
                                                         &visual.snapThreshold,
                                                         0.0f,
                                                         48.0f,
                                                         "%.1f px");
                       });
    }

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.visual.offset").data(), true) ) {
        // 采用全局统一最大标签宽度 maxLabelW
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.visual_offset").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           if ( ImGui::DragFloat("##VisualOffset",
                                                 &visual.visualOffset,
                                                 0.001f,
                                                 -0.5f,
                                                 0.5f,
                                                 "%.3f s") )
                               changed = true;
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
