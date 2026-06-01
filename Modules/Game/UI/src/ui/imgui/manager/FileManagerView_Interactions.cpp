#include "config/AppConfig.h"
#include "config/Utf8Path.h"
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
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <nfd.h>

#include <cmath>

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
                std::filesystem::path p = Config::utf8ToPath(drop.paths[0]);
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

/// @brief 渲染未打开项目时的文件浏览器占位内容。
/// @warning UI 热路径：未打开项目且子视图可见时每帧执行。
/// 避免文件系统扫描或高开销所有权操作。
void FileManagerView::renderEmptyProjectView(LayoutContext& layoutContext)
{
    CLayVBox    rootVBox;
    const char* openDirectoryLabel = TR("ui.file_manager.open_directory");
    const float openButtonWidth =
        std::ceil(ImGui::CalcTextSize(openDirectoryLabel).x +
                  ImGui::GetStyle().FramePadding.x * 2.0f + 2.0f);

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
        .addElement(
            "OpenDirButton",
            Sizing::Fixed(openButtonWidth),
            Sizing::Fixed(fh),
            [this, openDirectoryLabel](Clay_BoundingBox r, bool isHovered) {
                if ( ImGui::Button(openDirectoryLabel,
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
                [path, i](Clay_BoundingBox r, bool isHovered) {
                    std::filesystem::path p = Config::utf8ToPath(path);
                    std::string name        = Config::pathToUtf8(p.filename());
                    if ( name.empty() ) name = path;
                    const std::string itemId =
                        fmt::format("RecentProject_{}", i);
                    Utils::renderScrollingSelectable(
                        itemId,
                        name,
                        r.width,
                        r.height,
                        [p]() {
                            Event::OpenProjectEvent ev;
                            ev.m_projectPath = p;
                            Event::EventBus::instance().publish(ev);
                        },
                        path);
                    if ( ImGui::IsItemHovered() ) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
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
