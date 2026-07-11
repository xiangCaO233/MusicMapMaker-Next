#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "ui/imgui/manager/SettingsView.h"
#include "ui/utils/UIWidgetUtils.h"
#include <cstdint>
#include <string>

namespace MMM::UI
{

/// @brief 渲染调试设置页。
/// @warning UI 热路径：设置窗口打开且当前页为调试页时每帧执行。
/// 禁止加入文件系统扫描或重型资源重建。
void SettingsView::drawDebugSettings()
{
    auto& appConfig = Config::AppConfig::instance();
    auto& settings  = appConfig.getEditorSettings();
    auto& visual    = appConfig.getVisualConfig();
    bool  changed   = false;

    m_contentVBox.clear();
    m_contentVBox.setSpacing(6).setPadding(8, 8, 8, 8);
    size_t rowIndex     = 0;
    size_t sectionIndex = 0;

    const float maxLabelW =
        getCurrentTabLabelWidth(appConfig.getWindowContentScale());

    auto addHeader = [&](const char* label, bool defaultOpen) -> CLayVBox* {
        std::string baseIdStr = "DBG_S" + std::to_string(sectionIndex) + "_R" +
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
             addHeader(TR_CACHE("ui.settings.debug.rendering").data(), true) ) {
        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.debug.draw_hitboxes").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - ImGui::GetFrameHeight()) * 0.5f });
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##DebugDrawHitboxes", &visual.debugDrawHitboxes);
            });

        addSettingItem(
            *sec,
            rowIndex,
            TR_CACHE("ui.settings.debug.render_profile_logging").data(),
            maxLabelW,
            [&](Clay_BoundingBox r, bool) {
                ImGui::SetCursorScreenPos(
                    { r.x, r.y + (r.height - ImGui::GetFrameHeight()) * 0.5f });
                changed |= ::MMM::UI::FeedbackCheckbox(
                    "##RenderProfileLogging", &settings.renderProfileLogging);
            });
    }

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
