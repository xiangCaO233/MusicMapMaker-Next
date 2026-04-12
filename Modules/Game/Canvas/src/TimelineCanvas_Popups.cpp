#include "canvas/TimelineCanvas.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "logic/BeatmapSyncBuffer.h"

namespace MMM::Canvas
{

void TimelineCanvas::renderEventEditorPopup()
{
    ImGui::SetNextWindowSize(ImVec2(300, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15, 15));
    if ( ImGui::BeginPopupModal(
             "TimelineEventEditor",
             &m_isPopupOpen,
             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize) ) {
        std::string typeTitle =
            (m_editType == "BPM") ? TR("ui.timeline.event_type.bpm").data()
                                  : TR("ui.timeline.event_type.scroll").data();

        ImGui::Text("%s",
            TR_FMT("ui.timeline.event_editor.title", typeTitle).c_str());
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
            ImGui::InputDouble("##Value", &m_editValue, 0.01, 0.1, "%.3f");
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
    ImGui::SetNextWindowSize(ImVec2(350, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15, 15));

    if ( ImGui::BeginPopupModal("TimelineCreateEvent",
                                &m_isCreatePopupOpen,
                                ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_AlwaysAutoResize) ) {
        ImGui::TextUnformatted(TR("ui.timeline.event_creator.title").data());
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted(TR("ui.timeline.event_creator.pos_type").data());
        ImGui::RadioButton(
            m_isTimeSnapped
                ? TR("ui.timeline.event_creator.pos_click_snapped").data()
                : TR("ui.timeline.event_creator.pos_click").data(),
            &m_createPosType,
            0);
        ImGui::RadioButton(TR("ui.timeline.event_creator.pos_current").data(),
                           &m_createPosType,
                           1);

        double targetTime = (m_createPosType == 0)
                                ? m_createTimeSnapped
                                : m_currentSnapshot->currentTime;
        ImGui::Text("Time: %.3f s", targetTime);

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
        } else {
            ImGui::TextUnformatted(
                TR("ui.timeline.event_editor.scroll").data());
            ImGui::InputDouble("##ScrollValue", &m_createValue, 0.01, 0.1, "%.3f");
            ImGui::TextDisabled(
                "%s", TR("ui.timeline.event_editor.scroll_hint").data());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if ( ImGui::Button(TR("ui.timeline.event_creator.create").data(),
                           ImVec2(100, 0)) ) {
            ::MMM::TimingEffect type = (m_createType == 0)
                                           ? ::MMM::TimingEffect::BPM
                                           : ::MMM::TimingEffect::SCROLL;
            double finalValue = m_createValue;
            if ( type == ::MMM::TimingEffect::SCROLL ) {
                if ( m_createValue > 1e-6 ) {
                    finalValue = -100.0 / m_createValue;
                }
            }

            Event::EventBus::instance().publish(Event::LogicCommandEvent(
                Logic::CmdCreateTimelineEvent{ targetTime, type, finalValue }));
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

} // namespace MMM::Canvas
