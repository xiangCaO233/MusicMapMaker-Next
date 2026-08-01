#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIWidgetUtils.h"
#include <array>
#include <cstdint>
#include <fmt/format.h>
#include <string>
#include <string_view>

namespace MMM::UI
{

/// @brief 渲染视觉设置页。
void SettingsView::drawVisualSettings()
{
    auto& appConfig = Config::AppConfig::instance();
    auto& visual    = appConfig.getVisualConfig();
    bool  changed   = false;

    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    // 使用布局缓存中的统一标签列宽，避免设置页每帧重复测量全部标签。
    const float maxLabelW = getCurrentTabLabelWidth(
        Config::AppConfig::instance().getWindowContentScale());

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
             addHeader(TR_CACHE("ui.settings.visual.offset").data(), true) ) {
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.visual_offset").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackDragFloat("##VisualOffset",
                                                  &visual.visualOffset,
                                                  0.001f,
                                                  -0.5f,
                                                  0.5f,
                                                  "%.3f s") )
                    changed = true;
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.waveform_visual_offset").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackDragFloat("##WaveformVisualOffset",
                                                  &visual.waveformVisualOffset,
                                                  0.001f,
                                                  -0.5f,
                                                  0.5f,
                                                  "%.3f s") )
                    changed = true;
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.spectrum_visual_offset").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackDragFloat("##SpectrumVisualOffset",
                                                  &visual.spectrumVisualOffset,
                                                  0.001f,
                                                  -0.5f,
                                                  0.5f,
                                                  "%.3f s") )
                    changed = true;
            });
    }

    if ( auto* sec = addHeader(TR_CACHE("ui.settings.visual.beat_line").data(),
                               true) ) {
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.beat_line_before_first_timing").data(),
            maxLabelW,
            [&](Clay_BoundingBox, bool) {
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##BeatLineBeforeFirstTiming",
                    &visual.drawBeatLinesBeforeFirstTiming);
            });
    }

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.visual.preview").data(), true) ) {
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.preview_ratio").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ::MMM::UI::FeedbackSliderFloat(
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
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##EdgeSens",
                    &visual.previewConfig.edgeScrollSensitivity,
                    0.0f,
                    5.0f,
                    "%.4f");
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_left").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##MarginL",
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
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##MarginT", &visual.previewConfig.margin.top, 0.0f, 20.0f);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_margin_right").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##MarginR",
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
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##MarginB",
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
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##DrawBeatLines", &visual.previewConfig.drawBeatLines);
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.preview_draw_timing_lines").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##DrawTimingLines", &visual.previewConfig.drawTimingLines);
            });
    }

    if ( auto* sec = addHeader(
             TR_CACHE("ui.settings.visual.canvas_interaction").data(), true) ) {
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.timeline_zoom").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ::MMM::UI::FeedbackSliderFloat("##TimelineZoom",
                                                          &visual.timelineZoom,
                                                          0.1f,
                                                          5.0f,
                                                          "%.4fx");
                if ( ImGui::IsItemHovered() ) {
                    Utils::renderTooltip(
                        TR("ui.settings.visual.timeline_zoom_tooltip").data(),
                        Utils::TooltipDir::Right);
                }
            });
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.scroll_animation_duration").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetNextItemWidth(r.width);
                changed |= ::MMM::UI::FeedbackSliderFloat(
                    "##ScrollAnimationDuration",
                    &visual.scrollAnimationDuration,
                    0.0f,
                    0.5f,
                    "%.4f s");
                if ( ImGui::IsItemHovered() ) {
                    Utils::renderTooltip(
                        TR("ui.settings.visual.scroll_animation_tooltip")
                            .data(),
                        Utils::TooltipDir::Right);
                }
            });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.linear_scroll").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           changed |= ::MMM::UI::FeedbackCheckbox(
                               "##LinearScroll",
                               &visual.enableLinearScrollMapping);
                       });
        addSettingItem(*sec,
                       rowIndex,
                       TR_CACHE("ui.settings.visual.snap_threshold").data(),
                       maxLabelW,
                       [&](Clay_BoundingBox r, bool) {
                           ImGui::SetNextItemWidth(r.width);
                           changed |= ::MMM::UI::FeedbackSliderFloat(
                               "##SnapThreshold",
                               &visual.snapThreshold,
                               0.0f,
                               48.0f,
                               "%.4f px");
                       });
    }

    if ( auto* sec =
             addHeader(TR_CACHE("ui.settings.visual.spectrum").data(), true) ) {
        const std::array<Config::SpectrumDetailLevel, 6> detailLevels = {
            Config::SpectrumDetailLevel::Performance,
            Config::SpectrumDetailLevel::Balanced,
            Config::SpectrumDetailLevel::Fine,
            Config::SpectrumDetailLevel::Ultra,
            Config::SpectrumDetailLevel::Extreme,
            Config::SpectrumDetailLevel::Experimental
        };
        const std::array<std::string_view, 6> detailNames = {
            TR_CACHE("ui.settings.visual.spectrum_detail.performance").view,
            TR_CACHE("ui.settings.visual.spectrum_detail.balanced").view,
            TR_CACHE("ui.settings.visual.spectrum_detail.fine").view,
            TR_CACHE("ui.settings.visual.spectrum_detail.ultra").view,
            TR_CACHE("ui.settings.visual.spectrum_detail.extreme").view,
            TR_CACHE("ui.settings.visual.spectrum_detail.experimental").view
        };
        auto bytesToMiB = [](std::uint64_t bytes) {
            return static_cast<double>(bytes) / (1024.0 * 1024.0);
        };
        auto makeDetailLabel = [bytesToMiB](Config::SpectrumDetailLevel level,
                                            std::string_view            name) {
            const auto   profile   = Config::spectrumDetailProfile(level);
            const double stereoMiB = bytesToMiB(
                Config::estimateSpectrumTextureBytesPerMinute(level, 2, 1));
            const double monoMiB = bytesToMiB(
                Config::estimateSpectrumTextureBytesPerMinute(level, 1, 4));
            const std::string_view segmentsText =
                TR_CACHE(
                    "ui.settings.visual.spectrum_detail.segments_per_second")
                    .view;
            const std::string_view binsText =
                TR_CACHE("ui.settings.visual.spectrum_detail.bins").view;
            const std::string_view memoryPrefixText =
                TR_CACHE("ui.settings.visual.spectrum_detail.memory_prefix")
                    .view;
            const std::string_view minuteText =
                TR_CACHE("ui.settings.visual.spectrum_detail.minute").view;
            const std::string_view stereoText =
                TR_CACHE("ui.settings.visual.spectrum_detail.stereo").view;
            const std::string_view monoText =
                TR_CACHE("ui.settings.visual.spectrum_detail.bpm_mono").view;
            // 不把翻译文本作为 fmt 格式串解析，避免皮肤文案异常导致设置页崩溃。
            return fmt::format(
                "{} - {:.0f} {} x {} {}; {} {:.2f} MiB/{} ({}), {:.2f} "
                "MiB/{} ({})",
                name,
                profile.segmentsPerSecond,
                segmentsText,
                profile.frequencyBins,
                binsText,
                memoryPrefixText,
                stereoMiB,
                minuteText,
                stereoText,
                monoMiB,
                minuteText,
                monoText);
        };

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.visual.spectrum_detail").data(),
            maxLabelW,
            [&, detailLevels, detailNames, makeDetailLabel](Clay_BoundingBox r,
                                                            bool) {
                int currentIndex = 1;
                for ( size_t i = 0; i < detailLevels.size(); ++i ) {
                    if ( visual.spectrumDetailLevel == detailLevels[i] ) {
                        currentIndex = static_cast<int>(i);
                        break;
                    }
                }

                std::array<std::string, 6> labels;
                for ( size_t i = 0; i < detailLevels.size(); ++i ) {
                    labels[i] =
                        makeDetailLabel(detailLevels[i], detailNames[i]);
                }

                ImGui::SetNextItemWidth(r.width);
                if ( ::MMM::UI::FeedbackBeginCombo(
                         "##SpectrumDetail", labels[currentIndex].c_str()) ) {
                    for ( size_t i = 0; i < detailLevels.size(); ++i ) {
                        const bool selected =
                            currentIndex == static_cast<int>(i);
                        if ( ::MMM::UI::FeedbackSelectable(labels[i].c_str(),
                                                           selected) ) {
                            visual.spectrumDetailLevel = detailLevels[i];
                            changed                    = true;
                        }
                        if ( selected ) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ::MMM::UI::FeedbackEndCombo();
                }
                if ( ImGui::IsItemHovered() ) {
                    Utils::renderTooltip(
                        TR("ui.settings.visual.spectrum_detail.tooltip").data(),
                        Utils::TooltipDir::Right);
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
