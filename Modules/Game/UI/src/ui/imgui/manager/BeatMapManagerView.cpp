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

namespace MMM::UI
{

void BeatMapManagerView::onUpdate(LayoutContext& layoutContext,
                                  UIManager*     sourceManager)
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    auto& skinCfg = Config::SkinManager::instance();

    ImFont* fileManagerFont = skinCfg.getFont("filemanager");
    if ( fileManagerFont ) ImGui::PushFont(fileManagerFont);

    CLayVBox rootVBox;

    if ( !project ) {
        CLayHBox labelHBox;
        auto     fh = ImGui::GetFrameHeight();
        labelHBox.addSpring()
            .addElement(
                "InitialHint",
                Sizing::Grow(),
                Sizing::Fixed(fh),
                [=](Clay_BoundingBox r, bool isHovered) {
                    float offY = (r.height - ImGui::GetFontSize()) * 0.5f;
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offY);
                    ImVec2 textSize = ImGui::CalcTextSize(
                        TR("ui.beatmap_manager.initial_hint"));
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                         (r.width - textSize.x) * 0.5f);
                    ImGui::TextEx(TR("ui.beatmap_manager.initial_hint"));
                })
            .addSpring();

        rootVBox.setPadding(12, 12, 12, 12)
            .addLayout(
                "labelHBox", labelHBox, Sizing::Grow(), Sizing::Fixed(40));
        rootVBox.addSpring();
        rootVBox.render(layoutContext);

        if ( fileManagerFont ) ImGui::PopFont();
        return;
    }

    // 已打开项目时的界面
    CLayVBox listVBox;
    listVBox.setSpacing(4);

    listVBox.addElement(
        "BeatmapsHeader",
        Sizing::Grow(),
        Sizing::Fixed(24),
        [this](Clay_BoundingBox r, bool isHovered) {
            float indent = ImGui::CalcTextSize("AA").x;
            ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, indent);
            ImGui::SeparatorText(TR("ui.beatmap_manager.beatmaps").data());
            ImGui::PopStyleVar();
        });

    for ( const auto& beatmap : project->m_beatmaps ) {
        listVBox.addElement(
            "Beatmap_" + beatmap.m_filePath,
            Sizing::Grow(),
            Sizing::Fixed(28),
            [&beatmap, &engine, project](Clay_BoundingBox r, bool isHovered) {
                ImGui::Indent();
                std::string label = beatmap.m_name + "##" + beatmap.m_filePath;
                if ( ImGui::Selectable(label.c_str()) ) {
                    XINFO("Request to load beatmap: {}", beatmap.m_name);
                    auto fullPath =
                        project->m_projectRoot /
                        std::filesystem::path(reinterpret_cast<const char8_t*>(
                            beatmap.m_filePath.c_str()));
                    auto loadedBeatmap = std::make_shared<MMM::BeatMap>(
                        MMM::BeatMap::loadFromFile(fullPath));
                    engine.pushCommand(Logic::CmdLoadBeatmap{ loadedBeatmap });
                }
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("File: %s\nTrack: %s",
                                      beatmap.m_filePath.c_str(),
                                      beatmap.m_audioTrackId.c_str());
                }
                ImGui::Unindent();
            });
    }


    rootVBox.setPadding(12, 12, 12, 12)
        .setSpacing(8)
        .addElement(
            "BeatmapListArea",
            Sizing::Grow(),
            Sizing::Grow(),
            [&listVBox, &layoutContext](Clay_BoundingBox r, bool isHovered) {
                ImGui::BeginChild("BeatmapListChild",
                                  { r.width, r.height },
                                  false,
                                  ImGuiWindowFlags_HorizontalScrollbar);

                // 统一处理 LayoutContext
                // 的重映射，由于是在子窗口绘制，需要重定向
                ImVec2 oldStartPos = layoutContext.m_startPos;
                ImVec2 oldAvail    = layoutContext.m_avail;

                layoutContext.m_startPos = ImGui::GetCursorScreenPos();
                layoutContext.m_avail    = {
                    2000.0f, 10000.0f
                };  // 给予极大的垂直/水平空间以便显示滚动条

                listVBox.render(layoutContext);

                layoutContext.m_startPos = oldStartPos;
                layoutContext.m_avail    = oldAvail;

                ImGui::EndChild();
            });

    // 新建谱面按钮 (居中显示在列表下方)
    CLayHBox bottomBtnHBox;
    bottomBtnHBox.addSpring()
        .addElement(
            "Beatmap_CreateNew",
            Sizing::Fixed(32),
            Sizing::Fixed(32),
            [&engine, sourceManager](Clay_BoundingBox r, bool isHovered) {
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                      ImVec4(1, 1, 1, 0.1f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                      ImVec4(1, 1, 1, 0.2f));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));

                ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                     (r.width - 32.0f) * 0.5f);
                if ( ImGui::Button(ICON_MMM_PLUS, ImVec2(32, 32)) ) {
                    auto* wizard = sourceManager->getView<NewBeatmapWizard>(
                        "NewBeatmapWizard");
                    if ( wizard ) wizard->open();
                }

                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%s", TR_CACHE("ui.file.new_map").data());
                }
            })
        .addSpring();

    rootVBox.addLayout(
        "BottomBtnArea", bottomBtnHBox, Sizing::Grow(), Sizing::Fixed(32));

    rootVBox.render(layoutContext);

    if ( fileManagerFont ) ImGui::PopFont();
}

}  // namespace MMM::UI
