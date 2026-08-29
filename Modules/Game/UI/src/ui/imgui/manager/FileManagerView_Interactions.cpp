#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/translation/Translation.h"
#include "event/core/EventBus.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "ui/UIManager.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/manager/FileManagerView.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <nfd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <system_error>

namespace MMM::UI
{
namespace
{
/// @brief 将 ASCII 扩展名转换为小写。
/// @param value 输入扩展名。
/// @return 小写后的扩展名。
std::string toLowerAscii(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

/// @brief 判断拖拽文件是否为 zip 兼容谱面包。
/// @param path 拖拽文件路径。
/// @return 支持按临时项目打开时返回 true。
bool isTemporaryPackagePath(const std::filesystem::path& path)
{
    const auto extension = toLowerAscii(Config::pathToUtf8(path.extension()));
    return extension == ".zip" || extension == ".7z" || extension == ".mcz" ||
           extension == ".osz" || extension == ".mpk";
}
}  // namespace

void FileManagerView::handleDragDrop(UIManager* sourceManager)
{
    if ( m_pendingDrops.empty() ) return;

    bool isHovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    for ( const auto& drop : m_pendingDrops ) {
        if ( drop.paths.empty() ) {
            continue;
        }

        std::filesystem::path p = Config::utf8ToPath(drop.paths[0]);
        if ( !isHovered ) {
            continue;
        }

        if ( isTemporaryPackagePath(p) ) {
            XINFO("Package dropped: {}", Config::pathToUtf8(p));

            Event::OpenTemporaryProjectPackageEvent ev;
            ev.m_packagePath = p;
            Event::EventBus::instance().publish(ev);
            continue;
        }

        std::error_code filesystemError;
        filesystemError.clear();
        const bool isDirectory =
            std::filesystem::is_directory(p, filesystemError) &&
            !filesystemError;
        std::filesystem::path projectPath = isDirectory ? p : p.parent_path();

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
    const char* openDirectoryLabel =
        TR("ui.file_manager.open_directory").data();
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
                const char*  label = TR("ui.file_manager.initial_hint").data();
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
                if ( ::MMM::UI::FeedbackButton(openDirectoryLabel,
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
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t* outPath = nullptr;
        nfdresult_t  result  = NativeFileDialog::pickFolder(&outPath, nullptr);
        if ( result == NFD_OKAY ) {
            Event::OpenProjectEvent ev;
            ev.m_projectPath = Config::utf8ToPath(outPath);
            Event::EventBus::instance().publish(ev);
            NFD_FreePathU8(outPath);
        }
    } else {
        IGFD::FileDialogConfig fdConfig;
        fdConfig.path              = config.lastFilePickerPath;
        fdConfig.countSelectionMax = 1;
        fdConfig.flags             = ImGuiFileDialogFlags_Modal;
        const bool wasOpen =
            ImGuiFileDialog::Instance()->IsOpened("ProjectFolderPicker");
        ImGuiFileDialog::Instance()->OpenDialog(
            "ProjectFolderPicker",
            TR("ui.file_manager.open_directory").data(),
            nullptr,
            fdConfig);
        if ( !wasOpen &&
             ImGuiFileDialog::Instance()->IsOpened("ProjectFolderPicker") ) {
            ::MMM::UI::PlayPopupOpenFeedback();
        }
    }
}

}  // namespace MMM::UI
