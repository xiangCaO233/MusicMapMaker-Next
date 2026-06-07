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

#include <algorithm>
#include <cmath>
#include <cstdint>

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
                            ext == ".flac" || ext == ".opus" || ext == ".aac" ||
                            ext == ".m4a" ) {
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
    const auto metrics = getEmptyProjectViewMetrics(layoutContext.m_dpiScale);
    auto       toLayoutPixels = [](float value) {
        return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
    };

    const uint16_t layoutPadding = toLayoutPixels(metrics.padding);
    const uint16_t layoutGap     = toLayoutPixels(metrics.gap);

    CLayVBox    rootVBox;
    const char* openDirectoryLabel = TR("ui.file_manager.open_directory");
    const float openButtonWidth =
        std::ceil(ImGui::CalcTextSize(openDirectoryLabel).x +
                  ImGui::GetStyle().FramePadding.x * 2.0f + 2.0f);

    CLayHBox labelHBox;
    labelHBox.setAlignment(Alignment::Center());
    labelHBox.addSpring()
        .addElement(
            "InitialHint",
            Sizing::Grow(),
            Sizing::Fixed(metrics.buttonHeight),
            [=](Clay_BoundingBox r, bool isHovered) {
                const char*  label     = TR("ui.file_manager.initial_hint");
                const ImVec2 textSize  = ImGui::CalcTextSize(label);
                const float  textLineH = ImGui::GetTextLineHeight();
                const float  offsetX =
                    std::max(0.0f, (r.width - textSize.x) * 0.5f);
                const float offsetY =
                    std::max(0.0f, (r.height - textLineH) * 0.5f);
                ImGui::SetCursorScreenPos({ r.x + offsetX, r.y + offsetY });
                ImGui::TextUnformatted(label);
            })
        .addSpring();

    CLayHBox buttonHBox;
    buttonHBox.setAlignment(Alignment::Center());
    buttonHBox.addSpring()
        .addElement(
            "OpenDirButton",
            Sizing::Fixed(openButtonWidth),
            Sizing::Fixed(metrics.buttonHeight),
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
        recentVBox
            .setPadding(
                layoutPadding, 0, toLayoutPixels(metrics.recentTopPadding), 0)
            .setSpacing(layoutGap);
        recentVBox.addElement(
            "RecentTitle",
            Sizing::Grow(),
            Sizing::Fixed(metrics.recentTitleHeight),
            [](Clay_BoundingBox r, bool isHovered) {
                const float textLineH = ImGui::GetTextLineHeight();
                const float offsetY =
                    std::max(0.0f, (r.height - textLineH) * 0.5f);
                ImGui::SetCursorScreenPos({ r.x, r.y + offsetY });
                ImGui::TextDisabled("%s", TR("ui.file.open_recent").data());
            });

        for ( size_t i = 0; i < recent.size(); ++i ) {
            const auto& path = recent[i];
            recentVBox.addElement(
                fmt::format("RecentItem_{}", i),
                Sizing::Grow(),
                Sizing::Fixed(metrics.recentItemHeight),
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

    rootVBox
        .setPadding(layoutPadding, layoutPadding, layoutPadding, layoutPadding)
        .setSpacing(layoutGap)
        .addLayout("labelHBox",
                   labelHBox,
                   Sizing::Grow(),
                   Sizing::Fixed(metrics.hintRowHeight))
        .addLayout("buttonHBox",
                   buttonHBox,
                   Sizing::Grow(),
                   Sizing::Fixed(metrics.buttonRowHeight))
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
            ev.m_projectPath = Config::utf8ToPath(outPath);
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
