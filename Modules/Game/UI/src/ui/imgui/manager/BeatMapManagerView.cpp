#include "ui/imgui/manager/BeatMapManagerView.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/layout/box/CLayBox.h"

namespace MMM::UI
{

void BeatMapManagerView::onUpdate(LayoutContext& layoutContext,
                                  UIManager*     sourceManager)
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();

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
        return;
    }

    // 已打开项目时的界面
    CLayVBox listVBox;
    listVBox.setSpacing(4);

    listVBox.addElement(
        "BeatmapsHeader",
        Sizing::Grow(),
        Sizing::Fixed(24),
        [](Clay_BoundingBox r, bool isHovered) {
            ImGui::SeparatorText(TR("ui.beatmap_manager.beatmaps").data());
        });

    for ( const auto& beatmap : project->m_beatmaps ) {
        listVBox.addElement(
            "Beatmap_" + beatmap.m_name,
            Sizing::Grow(),
            Sizing::Fixed(28),
            [&beatmap, &engine, project](Clay_BoundingBox r, bool isHovered) {
                if ( ImGui::Selectable(beatmap.m_name.c_str()) ) {
                    XINFO("Request to load beatmap: {}", beatmap.m_name);
                    auto fullPath = project->m_projectRoot / beatmap.m_filePath;
                    auto loadedBeatmap = std::make_shared<MMM::BeatMap>(
                        MMM::BeatMap::loadFromFile(fullPath));
                    engine.pushCommand(Logic::CmdLoadBeatmap{ loadedBeatmap });
                }
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("File: %s\nTrack: %s",
                                      beatmap.m_filePath.c_str(),
                                      beatmap.m_audioTrackId.c_str());
                }
            });
    }

    rootVBox.setPadding(12, 12, 12, 12)
        .setSpacing(8)
        .addLayout("listVBox", listVBox, Sizing::Grow(), Sizing::Grow());

    rootVBox.addSpring();
    rootVBox.render(layoutContext);
}

}  // namespace MMM::UI
