#include "config/Utf8Path.h"
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
#include "ui/utils/UIWidgetUtils.h"

namespace MMM::UI
{

void FileManagerView::renderActiveProjectView(LayoutContext& layoutContext,
                                              UIManager*     sourceManager)
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    auto& skinCfg = Config::SkinManager::instance();

    m_currentRoot = project->m_projectRoot;

    CLayVBox treeVBox;
    treeVBox.setSpacing(2);

    // 1. Root 节点作为 CollapsingHeader
    treeVBox.addElement(
        "ProjectRootHeader",
        Sizing::Grow(),
        Sizing::Fixed(ImGui::GetFrameHeight()),
        [this, project](Clay_BoundingBox r, bool isHovered) {
            std::string rootName =
                Config::pathToUtf8(project->m_projectRoot.filename());
            std::string label = rootName;
            Utils::renderCollapsingHeader(label.c_str(), &m_showRoot, r);
            if ( ImGui::IsItemHovered() ) {
                std::string fullPath =
                    Config::pathToUtf8(project->m_projectRoot);
                ImGui::SetTooltip("%s", fullPath.c_str());
            }
        });

    if ( m_showRoot ) {
        treeVBox.addElement(
            "FileTree",
            Sizing::Grow(),
            Sizing::Grow(),
            [this, sourceManager, &layoutContext](Clay_BoundingBox r,
                                                  bool             isHovered) {
                ImGui::BeginChild("FileTreeChild",
                                  { r.width, r.height },
                                  false,
                                  ImGuiWindowFlags_None);

                ImVec2 oldStartPos = layoutContext.m_startPos;
                ImVec2 oldAvail    = layoutContext.m_avail;

                layoutContext.m_startPos = ImGui::GetCursorScreenPos();
                layoutContext.m_avail    = { r.width, 10000.0f };

                float indent = ImGui::CalcTextSize("AA").x;
                ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, indent);
                this->drawDirectoryRecursive(m_currentRoot, sourceManager);
                ImGui::PopStyleVar();

                layoutContext.m_startPos = oldStartPos;
                layoutContext.m_avail    = oldAvail;

                ImGui::EndChild();
            });
    }

    CLayVBox rootVBox;
    rootVBox.setPadding(12, 12, 12, 12)
        .setSpacing(8)
        .addLayout("treeVBox", treeVBox, Sizing::Grow(), Sizing::Grow());

    rootVBox.render(layoutContext);
}

void FileManagerView::drawDirectoryRecursive(const std::filesystem::path& path,
                                             UIManager* sourceManager)
{
    try {
        float availW = ImGui::GetContentRegionAvail().x;

        for ( const auto& entry : std::filesystem::directory_iterator(path) ) {
            const auto& p        = entry.path();
            std::string filename = Config::pathToUtf8(p.filename());
            std::string fullPath = Config::pathToUtf8(p);

            if ( filename.size() > 1 && filename[0] == '.' ) continue;

            bool isDir = entry.is_directory();
            bool open  = Utils::renderScrollingTreeNode(
                fullPath,
                filename,
                availW,
                24.0f,
                !isDir,
                [&]() {
                    // 文件点击逻辑 (仅对非目录项有效，目录由 TreeNode
                    // 自己处理展开)
                    if ( isDir ) return;

                    auto& engine  = Logic::EditorEngine::instance();
                    auto* project = engine.getCurrentProject();
                    if ( project ) {
                        auto relP = std::filesystem::relative(
                            p, project->m_projectRoot);
                        std::string relPath = Config::pathToUtf8(relP);
                        std::string ext     = Config::pathToUtf8(p.extension());

                        auto publishToggleEvent = [&](SideBarTab tab) {
                            Event::UISubViewToggleEvent evt;
                            evt.sourceUiName           = m_subViewName;
                            evt.uiManager              = sourceManager;
                            evt.targetFloatManagerName = "SideBarManager";
                            evt.subViewId              = TabToSubViewId(tab);
                            evt.showSubView            = true;
                            Event::EventBus::instance().publish(evt);
                        };

                        if ( ext == ".osu" || ext == ".imd" || ext == ".mc" ||
                             ext == ".mmm" ) {
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
                },
                fullPath);

            if ( isDir && open ) {
                drawDirectoryRecursive(p, sourceManager);
                ImGui::TreePop();
            }
        }
    } catch ( ... ) {
    }
}

}  // namespace MMM::UI
