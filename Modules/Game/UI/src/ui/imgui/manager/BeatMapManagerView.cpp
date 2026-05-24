#include "ui/imgui/manager/BeatMapManagerView.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>

namespace MMM::UI
{

void BeatMapManagerView::onUpdate(LayoutContext& layoutContext,
                                  UIManager*     sourceManager)
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    auto& skinCfg = Config::SkinManager::instance();

    float   dpiScale        = layoutContext.m_dpiScale;
    ImFont* fileManagerFont = skinCfg.getFont("filemanager");
    if ( fileManagerFont ) ImGui::PushFont(fileManagerFont);

    const float panelPadding = 12.0f * dpiScale;
    const float rowHeight    = 28.0f * dpiScale;

    if ( !project ) {
        const char* hint     = TR("ui.beatmap_manager.initial_hint").data();
        ImVec2      textSize = ImGui::CalcTextSize(hint);
        ImVec2      textPos  = {
            layoutContext.m_startPos.x +
                std::max(0.0f, layoutContext.m_avail.x - textSize.x) * 0.5f,
            layoutContext.m_startPos.y +
                std::max(0.0f, layoutContext.m_avail.y - textSize.y) * 0.5f
        };
        ImGui::SetCursorScreenPos(textPos);
        ImGui::TextDisabled("%s", hint);

        if ( fileManagerFont ) ImGui::PopFont();
        return;
    }

    float  footerH  = 44.0f * dpiScale;
    float  listH    = std::max(0.0f, layoutContext.m_avail.y - footerH);
    ImVec2 listPos  = { layoutContext.m_startPos.x + panelPadding,
                        layoutContext.m_startPos.y + panelPadding };
    ImVec2 listSize = { std::max(0.0f,
                                 layoutContext.m_avail.x - panelPadding * 2.0f),
                        std::max(0.0f, listH - panelPadding) };

    ImGui::SetCursorScreenPos(listPos);
    ImGui::BeginChild(
        "BeatmapListChild", listSize, false, ImGuiWindowFlags_None);

    ImVec2           headerPos = ImGui::GetCursorScreenPos();
    Clay_BoundingBox headerBox{ .x      = headerPos.x,
                                .y      = headerPos.y,
                                .width  = ImGui::GetContentRegionAvail().x,
                                .height = ImGui::GetFrameHeight() };
    Utils::renderCollapsingHeader(TR("ui.beatmap_manager.beatmaps").data(),
                                  &m_showBeatmapList,
                                  headerBox);

    if ( m_showBeatmapList ) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(project->m_beatmaps.size()), rowHeight);
        while ( clipper.Step() ) {
            for ( int row = clipper.DisplayStart; row < clipper.DisplayEnd;
                  ++row ) {
                const auto& beatmap = project->m_beatmaps[row];
                ImGui::Indent();
                std::string labelStr =
                    beatmap.m_name + " - " + beatmap.m_filePath;
                float availW = ImGui::GetContentRegionAvail().x;

                Utils::renderScrollingSelectable(
                    beatmap.m_filePath, labelStr, availW, rowHeight, [&]() {
                        XINFO("Request to load beatmap: {}", beatmap.m_name);
                        auto fullPath      = project->m_projectRoot /
                                             std::filesystem::path(
                                                 reinterpret_cast<const char8_t*>(
                                                     beatmap.m_filePath.c_str()));
                        auto loadedBeatmap = std::make_shared<MMM::BeatMap>(
                            MMM::BeatMap::loadFromFile(fullPath));
                        engine.pushCommand(
                            Logic::CmdLoadBeatmap{ loadedBeatmap });
                    });

                bool hovered  = ImGui::IsItemHovered();
                bool rclicked = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
                if ( hovered && rclicked ) {
                    m_manageBeatmapPath = beatmap.m_filePath;
                    m_openManageModal   = true;
                }

                if ( hovered ) {
                    ImGui::SetTooltip("File: %s\nTrack: %s",
                                      beatmap.m_filePath.c_str(),
                                      beatmap.m_audioTrackId.c_str());
                }
                ImGui::Unindent();
            }
        }
    }

    ImGui::EndChild();

    float  btnSize    = 32.0f * dpiScale;
    ImVec2 footerPos  = { layoutContext.m_startPos.x + panelPadding,
                          layoutContext.m_startPos.y + listH };
    ImVec2 footerSize = {
        std::max(0.0f, layoutContext.m_avail.x - panelPadding * 2.0f), footerH
    };
    ImVec2 buttonPos = {
        footerPos.x, footerPos.y + std::max(0.0f, footerSize.y - btnSize) * 0.5f
    };

    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

    ImGui::SetCursorScreenPos(buttonPos);
    ImDrawList* dl    = ImGui::GetWindowDrawList();
    ImVec4      bgCol = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
    bgCol.w *= 0.5f;
    float rounding = ImGui::GetStyle().FrameRounding;
    if ( ImGui::IsMouseHoveringRect(
             buttonPos,
             { buttonPos.x + footerSize.x, buttonPos.y + btnSize }) ) {
        bgCol.w *= 1.5f;
    }

    dl->AddRectFilled(buttonPos,
                      { buttonPos.x + footerSize.x, buttonPos.y + btnSize },
                      ImGui::ColorConvertFloat4ToU32(bgCol),
                      rounding);

    if ( ImGui::Button(ICON_MMM_PLUS, ImVec2(footerSize.x, btnSize)) ) {
        auto* wizard =
            sourceManager->getView<NewBeatmapWizard>("NewBeatmapWizard");
        if ( wizard ) wizard->open();
    }

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    if ( ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s", TR_CACHE("ui.file.new_map").data());
    }

    // --- 3. 谱面管理窗口 ---
    bool showBMModal = !m_manageBeatmapPath.empty();
    if ( showBMModal ) {
        std::string windowTitle =
            fmt::format("{} {}",
                        TR("ui.beatmap_manager.manage_title").data(),
                        m_manageBeatmapPath);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                ImGuiCond_Appearing,
                                ImVec2(0.5f, 0.5f));
        if ( m_openManageModal ) {
            ImGui::SetNextWindowSize({ 420 * dpiScale, 0 });
            m_openManageModal = false;
        }
        if ( ImGui::Begin(windowTitle.c_str(),
                          &showBMModal,
                          ImGuiWindowFlags_NoCollapse) ) {
            if ( !showBMModal ) {
                m_manageBeatmapPath = "";
            }

            // --- 使用 Clay 重构对话框内容 ---
            CLayVBox modalLayout;
            float    padding = 16 * dpiScale;
            modalLayout.setPadding(padding, padding, padding, padding);
            modalLayout.setSpacing(16 * dpiScale);

            // 1. 标题与信息 (移除了冗余 Text)
            modalLayout.addElement(
                "ModalSep",
                Sizing::Grow(),
                Sizing::Fixed(1),
                [=, this](Clay_BoundingBox r, bool) {
                    ImGui::GetWindowDrawList()->AddLine(
                        { r.x, r.y },
                        { r.x + r.width, r.y },
                        ImGui::GetColorU32(ImGuiCol_Separator));
                });

            // 2. 操作按钮区
            CLayHBox btnRow;
            btnRow.setAlignment(Alignment::Center());
            btnRow.setSpacing(12 * dpiScale);

            btnRow.addElement(
                "RemoveBtn",
                Sizing::Fixed(140 * dpiScale),
                Sizing::Fixed(32 * dpiScale),
                [=](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    if ( ImGui::Button(
                             TR("ui.beatmap_manager.remove_beatmap").data(),
                             { r.width, r.height }) ) {
                        ImGui::OpenPopup("RemoveBeatmapConfirm");
                    }
                });

            btnRow.addElement(
                "CancelBtn",
                Sizing::Fixed(100 * dpiScale),
                Sizing::Fixed(32 * dpiScale),
                [=, this](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    if ( ImGui::Button(TR("ui.common.cancel").data(),
                                       { r.width, r.height }) ) {
                        m_manageBeatmapPath = "";
                    }
                });

            modalLayout.addLayout("BtnRowLayout",
                                  btnRow,
                                  Sizing::Grow(),
                                  Sizing::Fixed(32 * dpiScale));

            // 渲染布局
            ImVec2 modalSize = modalLayout.renderInCurrent(
                ImGui::GetCursorScreenPos(),
                { ImGui::GetContentRegionAvail().x, 0 });
            ImGui::Dummy(modalSize);

            // --- 二次确认弹窗 ---
            {
                static bool wasOpen = false;
                bool        isOpen = ImGui::IsPopupOpen("RemoveBeatmapConfirm");
                if ( isOpen && !wasOpen ) {
                    ImGui::SetNextWindowPos(
                        ImGui::GetMainViewport()->GetCenter(),
                        ImGuiCond_Always,
                        ImVec2(0.5f, 0.5f));
                }
                wasOpen = isOpen;
            }
            if ( ImGui::BeginPopupModal(
                     "RemoveBeatmapConfirm", nullptr, ImGuiWindowFlags_None) ) {
                ImGui::Text("%s",
                            TR("ui.beatmap_manager.remove_confirm").data());
                ImGui::Spacing();
                if ( ImGui::Button(TR("ui.common.confirm").data(),
                                   { 100 * dpiScale, 0 }) ) {
                    engine.pushCommand(
                        Logic::CmdRemoveBeatmap{ m_manageBeatmapPath });
                    m_manageBeatmapPath = "";
                }
                ImGui::SameLine();
                if ( ImGui::Button(TR("ui.common.cancel").data(),
                                   { 100 * dpiScale, 0 }) ) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::End();
        }
    }

    if ( fileManagerFont ) ImGui::PopFont();
}

}  // namespace MMM::UI
