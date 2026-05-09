#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "ui/UIManager.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/manager/FileManagerView.h"
#include "ui/layout/box/CLayBox.h"
#include <ImGuiFileDialog.h>
#include <nfd.h>

namespace MMM::UI
{

void FileManagerView::handleDragDrop(UIManager* sourceManager)
{
    if ( m_pendingDrops.empty() ) return;

    bool isHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    if ( isHovered ) {
        for ( const auto& drop : m_pendingDrops ) {
            if ( !drop.paths.empty() ) {
                std::filesystem::path p(drop.paths[0]);
                std::filesystem::path projectPath =
                    std::filesystem::is_directory(p) ? p : p.parent_path();

                XINFO("File dropped on FileManager: {}, opening project: {}",
                      Config::pathToUtf8(p),
                      Config::pathToUtf8(projectPath));

                Event::OpenProjectEvent ev;
                ev.m_projectPath = projectPath;
                Event::EventBus::instance().publish(ev);

                auto       ext       = Config::pathToUtf8(p.extension());
                SideBarTab targetTab = SideBarTab::FileExplorer;
                if ( ext == ".osu" || ext == ".imd" || ext == ".mc" ) {
                    targetTab = SideBarTab::BeatMapExplorer;
                } else if ( ext == ".mp3" || ext == ".ogg" || ext == ".wav" ||
                            ext == ".flac" ) {
                    targetTab = SideBarTab::AudioExplorer;
                }

                Event::UISubViewToggleEvent evt;
                evt.sourceUiName           = m_subViewName;
                evt.uiManager              = sourceManager;
                evt.targetFloatManagerName = "SideBarManager";
                evt.subViewId              = TabToSubViewId(targetTab);
                evt.showSubView            = true;
                Event::EventBus::instance().publish(evt);
            }
        }
    }
    m_pendingDrops.clear();
}

void FileManagerView::renderEmptyProjectView(LayoutContext& layoutContext)
{
    auto&    skinCfg = Config::SkinManager::instance();
    CLayVBox rootVBox;

    CLayHBox labelHBox;
    auto     fh = ImGui::GetFrameHeight();
    labelHBox.addSpring()
        .addElement("InitialHint",
                    Sizing::Grow(),
                    Sizing::Fixed(fh),
                    [=](Clay_BoundingBox r, bool isHovered) {
                        float offY = (r.height - ImGui::GetFontSize()) * 0.5f;
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offY);
                        ImVec2 textSize = ImGui::CalcTextSize(
                            TR("ui.file_manager.initial_hint"));
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                             (r.width - textSize.x) * 0.5f);
                        ImGui::TextEx(TR("ui.file_manager.initial_hint"));
                    })
        .addSpring();

    CLayHBox buttonHBox;
    buttonHBox.addSpring()
        .addElement("OpenDirButton",
                    Sizing::Grow(),
                    Sizing::Fixed(fh),
                    [this](Clay_BoundingBox r, bool isHovered) {
                        if ( ImGui::Button(TR("ui.file_manager.open_directory"),
                                           { r.width, r.height }) ) {
                            this->openFolderPicker();
                        }
                    })
        .addSpring();

    CLayVBox    recentVBox;
    const auto& recent =
        Config::AppConfig::instance().getEditorConfig().recentProjects;

    if ( !recent.empty() ) {
        recentVBox.setPadding(12, 0, 12, 0).setSpacing(8);
        recentVBox.addElement("RecentTitle",
                              Sizing::Grow(),
                              Sizing::Fixed(20),
                              [](Clay_BoundingBox r, bool isHovered) {
                                  ImGui::TextDisabled(
                                      "%s", TR("ui.file.open_recent").data());
                              });

        for ( size_t i = 0; i < recent.size(); ++i ) {
            const auto& path = recent[i];
            recentVBox.addElement(
                fmt::format("RecentItem_{}", i),
                Sizing::Grow(),
                Sizing::Fixed(20),
                [path, &skinCfg](Clay_BoundingBox r, bool isHovered) {
                    std::filesystem::path p = Config::utf8ToPath(path);
                    std::string name        = Config::pathToUtf8(p.filename());
                    if ( name.empty() ) name = path;
                    ImVec4 col = ImGui::GetStyleColorVec4(ImGuiCol_Text);
                    if ( isHovered )
                        col = ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered];

                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    ImGui::TextUnformatted(name.c_str());
                    ImGui::PopStyleColor();

                    ImVec2 min = ImGui::GetItemRectMin();
                    ImVec2 max = ImGui::GetItemRectMax();
                    min.y      = max.y;
                    ImGui::GetWindowDrawList()->AddLine(
                        min, max, ImGui::ColorConvertFloat4ToU32(col), 1.0f);

                    if ( isHovered ) {
                        ImGui::SetTooltip("%s", path.c_str());
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }
                    if ( ImGui::IsItemClicked() ) {
                        Event::OpenProjectEvent ev;
                        ev.m_projectPath = p;
                        Event::EventBus::instance().publish(ev);
                    }
                });
        }
    }

    rootVBox.setPadding(12, 12, 12, 12)
        .setSpacing(12)
        .addLayout("labelHBox", labelHBox, Sizing::Grow(), Sizing::Fixed(40))
        .addLayout("buttonHBox", buttonHBox, Sizing::Grow(), Sizing::Fixed(40))
        .addLayout("recentVBox", recentVBox, Sizing::Grow(), Sizing::Grow());

    rootVBox.addSpring();
    rootVBox.render(layoutContext);
}

void FileManagerView::openFolderPicker()
{
    auto& config = Config::AppConfig::instance().getEditorSettings();
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        nfdu8char_t* outPath = nullptr;
        nfdresult_t  result  = NFD_PickFolder(&outPath, nullptr);
        if ( result == NFD_OKAY ) {
            Event::OpenProjectEvent ev;
            ev.m_projectPath = std::filesystem::path(
                reinterpret_cast<const char8_t*>(outPath));
            Event::EventBus::instance().publish(ev);
            NFD_FreePath(outPath);
        }
    } else {
        IGFD::FileDialogConfig fdConfig;
        fdConfig.path              = config.lastFilePickerPath;
        fdConfig.countSelectionMax = 1;
        fdConfig.flags             = ImGuiFileDialogFlags_Default;
        ImGuiFileDialog::Instance()->OpenDialog(
            "ProjectFolderPicker",
            TR("ui.file_manager.open_directory"),
            nullptr,
            fdConfig);
    }
}

}  // namespace MMM::UI
