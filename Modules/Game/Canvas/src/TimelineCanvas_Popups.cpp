#include "canvas/TimelineCanvas.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"

namespace MMM::Canvas
{

void TimelineCanvas::renderEventEditorPopup()
{
    static bool wasOpen = false;
    bool        isOpen  = ImGui::IsPopupOpen("TimelineEventEditor");
    if ( isOpen && !wasOpen ) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Always,
                                ImVec2(0.5f, 0.5f));
    }
    wasOpen = isOpen;

    ImGui::SetNextWindowSize(ImVec2(300, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15, 15));
    if ( ImGui::BeginPopupModal(
             "TimelineEventEditor", &m_isPopupOpen, ImGuiWindowFlags_None) ) {
        std::string typeTitle =
            (m_editType == "BPM") ? TR("ui.timeline.event_type.bpm").data()
                                  : TR("ui.timeline.event_type.scroll").data();

        ImGui::Text(
            "%s", TR_FMT("ui.timeline.event_editor.title", typeTitle).c_str());
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted(TR("ui.timeline.event_editor.timestamp").data());
        ImGui::InputDouble("##Time", &m_editTime, 0.001, 0.01, "%.3f");

        if ( m_editType == "BPM" ) {
            ImGui::TextUnformatted(TR("ui.timeline.event_editor.bpm").data());
            ImGui::InputDouble("##Value", &m_editValue, 0.1, 1.0, "%.2f");
        } else {
            ImGui::TextUnformatted(
                TR("ui.timeline.event_editor.scroll").data());
            ImGui::InputDouble("##Value", &m_editValue, 0.01, 0.1, "%.4f");
            ImGui::TextDisabled(
                "%s", TR("ui.timeline.event_editor.scroll_hint").data());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if ( ImGui::Button(TR("ui.timeline.event_editor.apply").data(),
                           ImVec2(80, 0)) ) {
            double finalValue = m_editValue;
            if ( m_editType == "Scroll" ) {
                if ( m_editValue > 1e-6 ) {
                    finalValue = -100.0 / m_editValue;
                }
            }

            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdUpdateTimelineEvent{
                    m_editingEntity, m_editTime, finalValue }));
            ImGui::CloseCurrentPopup();
            m_isPopupOpen = false;
        }

        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.timeline.event_editor.delete").data(),
                           ImVec2(80, 0)) ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdDeleteTimelineEvent{ m_editingEntity }));
            ImGui::CloseCurrentPopup();
            m_isPopupOpen = false;
        }

        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.timeline.event_editor.cancel").data(),
                           ImVec2(80, 0)) ) {
            ImGui::CloseCurrentPopup();
            m_isPopupOpen = false;
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void TimelineCanvas::renderEventCreationPopup()
{
    static bool wasOpen = false;
    bool        isOpen  = ImGui::IsPopupOpen("TimelineCreateEvent");
    if ( isOpen && !wasOpen ) {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Always,
                                ImVec2(0.5f, 0.5f));
    }
    wasOpen = isOpen;

    ImGui::SetNextWindowSize(ImVec2(350, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15, 15));

    if ( ImGui::BeginPopupModal("TimelineCreateEvent",
                                &m_isCreatePopupOpen,
                                ImGuiWindowFlags_None) ) {
        ImGui::TextUnformatted(TR("ui.timeline.event_creator.title").data());
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted(TR("ui.timeline.event_creator.pos_type").data());
        if ( ImGui::RadioButton(
                 m_isTimeSnapped
                     ? TR("ui.timeline.event_creator.pos_click_snapped").data()
                     : TR("ui.timeline.event_creator.pos_click").data(),
                 &m_createPosType,
                 0) ) {
            m_createTimeManual =
                m_isTimeSnapped ? m_createTimeSnapped : m_createTimeRaw;
        }
        if ( ImGui::RadioButton(
                 TR("ui.timeline.event_creator.pos_current").data(),
                 &m_createPosType,
                 1) ) {
            m_createTimeManual = m_currentSnapshot->currentTime;
        }

        ImGui::TextUnformatted(TR("ui.timeline.event_editor.timestamp").data());
        ImGui::InputDouble(
            "##CreateTime", &m_createTimeManual, 0.001, 0.01, "%.3f");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted(TR("ui.timeline.event_creator.type").data());
        if ( ImGui::RadioButton("BPM", &m_createType, 0) ) {
            m_createValue = 120.0;
        }
        ImGui::SameLine();
        if ( ImGui::RadioButton("Scroll", &m_createType, 1) ) {
            m_createValue = 1.0;
        }

        ImGui::Spacing();
        if ( m_createType == 0 ) {
            ImGui::TextUnformatted(TR("ui.timeline.event_editor.bpm").data());
            ImGui::InputDouble("##BPMValue", &m_createValue, 0.1, 1.0, "%.2f");
            ImGui::Spacing();
            ImGui::Checkbox(TR("ui.timeline.event_creator.keep_speed").data(),
                            &m_keepSpeedOnBpmChange);
        } else {
            ImGui::TextUnformatted(
                TR("ui.timeline.event_editor.scroll").data());
            ImGui::InputDouble(
                "##ScrollValue", &m_createValue, 0.01, 0.1, "%.3f");
            ImGui::TextDisabled(
                "%s", TR("ui.timeline.event_editor.scroll_hint").data());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if ( ImGui::Button(TR("ui.timeline.event_creator.create").data(),
                           ImVec2(100, 0)) ) {
            ::MMM::TimingEffect type       = (m_createType == 0)
                                                 ? ::MMM::TimingEffect::BPM
                                                 : ::MMM::TimingEffect::SCROLL;
            double              finalValue = m_createValue;
            if ( type == ::MMM::TimingEffect::SCROLL ) {
                if ( m_createValue > 1e-6 ) {
                    finalValue = -100.0 / m_createValue;
                }
            }

            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdCreateTimelineEvent{
                    m_createTimeManual, type, finalValue }));

            if ( type == ::MMM::TimingEffect::BPM && m_keepSpeedOnBpmChange ) {
                double refBpm = 120.0;
                if ( auto session =
                         Logic::EditorEngine::instance().getActiveSession() ) {
                    if ( auto beatmap = session->getContext().currentBeatmap ) {
                        if ( beatmap->m_baseMapMetadata.preference_bpm > 0.0 ) {
                            refBpm = beatmap->m_baseMapMetadata.preference_bpm;
                        }
                    }
                }
                double scrollSpeed = refBpm / m_createValue;
                double finalScrollValue =
                    scrollSpeed > 1e-6 ? (-100.0 / scrollSpeed) : -100.0;
                Event::EventBus::instance().publish(Event::LogicCommandEvent(
                    Logic::CmdCreateTimelineEvent{ m_createTimeManual,
                                                   ::MMM::TimingEffect::SCROLL,
                                                   finalScrollValue }));
            }

            ImGui::CloseCurrentPopup();
            m_isCreatePopupOpen = false;
        }

        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.timeline.event_editor.cancel").data(),
                           ImVec2(100, 0)) ) {
            ImGui::CloseCurrentPopup();
            m_isCreatePopupOpen = false;
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

/// @brief 渲染可批量编辑时间点的表格窗口（非模态）
void TimelineCanvas::renderTimingPointsTableWindow()
{
    if ( !m_isTableWindowOpen ) return;

    ImGui::SetNextWindowSize(ImVec2(650, 450), ImGuiCond_FirstUseEver);

    if ( ImGui::Begin(TR("ui.timeline.timing_points_table.title").data(),
                      &m_isTableWindowOpen) ) {
        if ( !m_currentSnapshot || !m_currentSnapshot->hasBeatmap ) {
            ImGui::TextDisabled("当前未加载任何谱面");
            ImGui::End();
            return;
        }

        auto elements = m_currentSnapshot->timelineElements;
        std::sort(elements.begin(),
                  elements.end(),
                  [](const auto& a, const auto& b) { return a.time < b.time; });

        // 顶层工具栏
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(TR("ui.timeline.event_creator.title").data());
        ImGui::SameLine();
        if ( ImGui::Button("添加 BPM") ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdCreateTimelineEvent{ m_currentSnapshot->currentTime,
                                               ::MMM::TimingEffect::BPM,
                                               120.0 }));
        }
        ImGui::SameLine();
        if ( ImGui::Button("添加流速 (SV)") ) {
            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdCreateTimelineEvent{ m_currentSnapshot->currentTime,
                                               ::MMM::TimingEffect::SCROLL,
                                               -100.0 }));
        }

        ImGui::Separator();

        // 批量修改工具
        if ( ImGui::TreeNode("批量修改工具##BulkTools") ) {
            // 批量偏移
            static double bulkOffsetValue = 0.0;
            ImGui::TextUnformatted("批量时间偏移 (秒):");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputDouble(
                "##BulkOffsetInput", &bulkOffsetValue, 0.001, 0.01, "%.3f");
            ImGui::SameLine();
            if ( ImGui::Button("应用时间偏移") &&
                 std::abs(bulkOffsetValue) > 1e-6 ) {
                for ( const auto& el : elements ) {
                    entt::entity ent =
                        (el.effects & Logic::System::SCROLL_EFFECT_BPM)
                            ? el.bpmEntity
                            : el.scrollEntity;
                    double rawVal =
                        (el.effects & Logic::System::SCROLL_EFFECT_BPM)
                            ? el.bpmValue
                            : el.scrollValue;
                    Event::EventBus::instance().publish(
                        Event::LogicCommandEvent(Logic::CmdUpdateTimelineEvent{
                            ent, el.time + bulkOffsetValue, rawVal }));
                }
                bulkOffsetValue = 0.0;
            }

            // 批量缩放
            static double bulkScaleValue = 1.0;
            ImGui::TextUnformatted("批量流速缩放倍率:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputDouble(
                "##BulkScaleInput", &bulkScaleValue, 0.01, 0.1, "%.2f");
            ImGui::SameLine();
            if ( ImGui::Button("应用流速缩放") &&
                 std::abs(bulkScaleValue - 1.0) > 1e-6 ) {
                for ( const auto& el : elements ) {
                    if ( el.effects & Logic::System::SCROLL_EFFECT_SCROLL ) {
                        double dispScroll = (el.scrollValue < -1e-6)
                                                ? (-100.0 / el.scrollValue)
                                                : el.scrollValue;
                        double newDisp    = dispScroll * bulkScaleValue;
                        double newVal =
                            newDisp > 1e-6 ? (-100.0 / newDisp) : -100.0;
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdUpdateTimelineEvent{
                                    el.scrollEntity, el.time, newVal }));
                    }
                }
                bulkScaleValue = 1.0;
            }

            ImGui::TreePop();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // 渲染表格
        if ( ImGui::BeginTable(
                 "TimingPointsTableMain",
                 5,
                 ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                     ImGuiTableFlags_Hideable | ImGuiTableFlags_RowBg |
                     ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV |
                     ImGuiTableFlags_ScrollY,
                 ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing())) ) {
            ImGui::TableSetupColumn(
                "序号", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("时间戳 (秒)",
                                    ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(
                "类型", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("数值", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn(
                "操作", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableHeadersRow();

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(elements.size()));
            while ( clipper.Step() ) {
                for ( int idx = clipper.DisplayStart; idx < clipper.DisplayEnd;
                      ++idx ) {
                    const auto& el         = elements[idx];
                    int         displayIdx = idx + 1;

                    ImGui::TableNextRow();

                    // Column 0: 序号
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("#%d", displayIdx);

                    // Column 1: 时间戳 (秒)
                    ImGui::TableSetColumnIndex(1);
                    double tVal = el.time;
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    std::string tId = fmt::format("##T_{}", displayIdx);
                    if ( ImGui::InputDouble(
                             tId.c_str(),
                             &tVal,
                             0.001,
                             0.01,
                             "%.3f",
                             ImGuiInputTextFlags_EnterReturnsTrue) ) {
                        entt::entity ent =
                            (el.effects & Logic::System::SCROLL_EFFECT_BPM)
                                ? el.bpmEntity
                                : el.scrollEntity;
                        double rawVal =
                            (el.effects & Logic::System::SCROLL_EFFECT_BPM)
                                ? el.bpmValue
                                : el.scrollValue;
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdUpdateTimelineEvent{
                                    ent, tVal, rawVal }));
                    }

                    // Column 2: 类型
                    ImGui::TableSetColumnIndex(2);
                    if ( el.effects & Logic::System::SCROLL_EFFECT_BPM ) {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                        ImGui::TextUnformatted("BPM");
                        ImGui::PopStyleColor();
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text,
                                              ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                        ImGui::TextUnformatted("流速 (SV)");
                        ImGui::PopStyleColor();
                    }

                    // Column 3: 数值
                    ImGui::TableSetColumnIndex(3);
                    bool isBpm =
                        (el.effects & Logic::System::SCROLL_EFFECT_BPM);
                    double vVal = isBpm ? el.bpmValue
                                        : ((el.scrollValue < -1e-6)
                                               ? (-100.0 / el.scrollValue)
                                               : el.scrollValue);
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    std::string vId = fmt::format("##V_{}", displayIdx);
                    if ( ImGui::InputDouble(
                             vId.c_str(),
                             &vVal,
                             isBpm ? 0.1 : 0.01,
                             isBpm ? 1.0 : 0.1,
                             isBpm ? "%.2f" : "%.4f",
                             ImGuiInputTextFlags_EnterReturnsTrue) ) {
                        entt::entity ent =
                            isBpm ? el.bpmEntity : el.scrollEntity;
                        double finalValue = vVal;
                        if ( !isBpm ) {
                            if ( vVal > 1e-6 ) {
                                finalValue = -100.0 / vVal;
                            }
                        }
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdUpdateTimelineEvent{
                                    ent, el.time, finalValue }));
                    }

                    // Column 4: 操作
                    ImGui::TableSetColumnIndex(4);
                    std::string seekId =
                        fmt::format("跳转##Seek_{}", displayIdx);
                    if ( ImGui::Button(seekId.c_str()) ) {
                        float visualOffset = Config::AppConfig::instance()
                                                 .getVisualConfig()
                                                 .getEffectiveVisualOffset();
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdSeek{ el.time - visualOffset }));
                    }
                    ImGui::SameLine();
                    std::string delId = fmt::format("删除##Del_{}", displayIdx);
                    if ( ImGui::Button(delId.c_str()) ) {
                        entt::entity ent =
                            isBpm ? el.bpmEntity : el.scrollEntity;
                        Event::EventBus::instance().publish(
                            Event::LogicCommandEvent(
                                Logic::CmdDeleteTimelineEvent{ ent }));
                    }
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

}  // namespace MMM::Canvas
