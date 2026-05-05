#include "config/skin/SkinConfig.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/UIManager.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/imgui/manager/FileManagerView.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIThemeUtils.h"

namespace MMM::UI
{

void FileManagerView::renderActiveProjectView(LayoutContext& layoutContext,
                                              UIManager*     sourceManager)
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    auto& skinCfg = Config::SkinManager::instance();

    m_currentRoot = project->m_projectRoot;

    CLayHBox titleHBox;
    titleHBox.addElement(
        "ProjectTitle",
        Sizing::Grow(),
        Sizing::Fixed(ImGui::GetFrameHeight()),
        [project](Clay_BoundingBox r, bool isHovered) {
            ImVec4 highlightCol = Utils::UIThemeUtils::getHighlightColor();
            auto        u8Name = project->m_projectRoot.filename().u8string();
            std::string rootName(reinterpret_cast<const char*>(u8Name.c_str()),
                                 u8Name.size());
            ImGui::TextColored(highlightCol,
                               "Root: %s",
                               rootName.c_str());
            if ( ImGui::IsItemHovered() ) {
                auto        u8Full = project->m_projectRoot.u8string();
                std::string fullPath(
                    reinterpret_cast<const char*>(u8Full.c_str()),
                    u8Full.size());
                ImGui::SetTooltip("%s", fullPath.c_str());
            }
        });

    CLayVBox treeVBox;
    treeVBox.setPadding(4, 4, 4, 4)
        .addElement(
            "FileTree",
            Sizing::Grow(),
            Sizing::Grow(),
            [this, sourceManager, &layoutContext](Clay_BoundingBox r,
                                                  bool             isHovered) {
                ImGui::SetNextWindowContentSize(ImVec2(2000.0f, 0.0f));
                ImGui::BeginChild("FileTreeChild",
                                  { 0, 0 },
                                  false,
                                  ImGuiWindowFlags_HorizontalScrollbar);

                ImVec2 oldAvail       = layoutContext.m_avail;
                layoutContext.m_avail = { 2000.0f, 10000.0f };
                float indent          = ImGui::CalcTextSize("AA").x;
                ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, indent);
                this->drawDirectoryRecursive(m_currentRoot, sourceManager);
                ImGui::PopStyleVar();
                layoutContext.m_avail = oldAvail;
                ImGui::EndChild();
            });

    CLayVBox rootVBox;
    rootVBox.setPadding(8, 8, 8, 8)
        .setSpacing(4)
        .addLayout("titleHBox", titleHBox, Sizing::Grow(), Sizing::Fixed(24))
        .addLayout("treeVBox", treeVBox, Sizing::Grow(), Sizing::Grow());

    rootVBox.render(layoutContext);
}

void FileManagerView::drawDirectoryRecursive(const std::filesystem::path& path,
                                             UIManager* sourceManager)
{
    try {
        for ( const auto& entry : std::filesystem::directory_iterator(path) ) {
            const auto& p  = entry.path();
            auto        u8 = p.filename().u8string();
            std::string filename(reinterpret_cast<const char*>(u8.c_str()),
                                 u8.size());
            if ( filename.size() > 1 && filename[0] == '.' ) continue;

            if ( entry.is_directory() ) {
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                           ImGuiTreeNodeFlags_OpenOnDoubleClick;
                bool open = ImGui::TreeNodeEx(filename.c_str(), flags);
                if ( open ) {
                    drawDirectoryRecursive(p, sourceManager);
                    ImGui::TreePop();
                }
            } else {
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf |
                                           ImGuiTreeNodeFlags_NoTreePushOnOpen;
                ImGui::TreeNodeEx(filename.c_str(), flags);
                if ( ImGui::IsItemClicked() ) {
                    auto& engine  = Logic::EditorEngine::instance();
                    auto* project = engine.getCurrentProject();
                    if ( project ) {
                        auto relP = std::filesystem::relative(
                            p, project->m_projectRoot);
                        auto        relU8 = relP.generic_u8string();
                        std::string relPath(
                            reinterpret_cast<const char*>(relU8.c_str()),
                            relU8.size());
                        auto        extU8 = p.extension().u8string();
                        std::string ext(
                            reinterpret_cast<const char*>(extU8.c_str()),
                            extU8.size());

                        auto publishToggleEvent = [&](SideBarTab tab) {
                            Event::UISubViewToggleEvent evt;
                            evt.sourceUiName           = m_subViewName;
                            evt.uiManager              = sourceManager;
                            evt.targetFloatManagerName = "SideBarManager";
                            evt.subViewId              = TabToSubViewId(tab);
                            evt.showSubView            = true;
                            Event::EventBus::instance().publish(evt);
                        };

                        if ( ext == ".osu" || ext == ".imd" || ext == ".mc" || ext == ".mmm" ) {
                            publishToggleEvent(SideBarTab::BeatMapExplorer);
                            for ( const auto& bm : project->m_beatmaps ) {
                                if ( bm.m_filePath == relPath ) {
                                    auto loadedBeatmap =
                                        std::make_shared<MMM::BeatMap>(
                                            MMM::BeatMap::loadFromFile(p));
                                    engine.pushCommand(
                                        Logic::CmdLoadBeatmap{ loadedBeatmap });
                                    break;
                                }
                            }
                        } else if ( ext == ".mp3" || ext == ".wav" ||
                                    ext == ".ogg" || ext == ".flac" ) {
                            publishToggleEvent(SideBarTab::AudioExplorer);
                            for ( const auto& audio :
                                  project->m_audioResources ) {
                                if ( audio.m_path == relPath ) {
                                    std::string viewName =
                                        "TrackController_" + audio.m_id;
                                    if ( !sourceManager
                                              ->getView<AudioTrackControllerUI>(
                                                  viewName) ) {
                                        auto controller = std::make_unique<
                                            AudioTrackControllerUI>(
                                            audio.m_id,
                                            audio.m_id,
                                            audio.m_type == AudioTrackType::Main
                                                ? AudioTrackControllerUI::
                                                      TrackType::Main
                                                : AudioTrackControllerUI::
                                                      TrackType::Effect);
                                        sourceManager->registerView(
                                            viewName, std::move(controller));
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    } catch ( ... ) {
    }
}

}  // namespace MMM::UI
