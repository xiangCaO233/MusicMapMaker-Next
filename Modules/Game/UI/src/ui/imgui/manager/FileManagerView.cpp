#include "ui/imgui/manager/FileManagerView.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "ui/layout/box/CLayBox.h"
#include <filesystem>

// 新添加的第三方库
#include <ImGuiFileDialog.h>
#include <nfd.h>

namespace MMM::UI
{

FileManagerView::FileManagerView(const std::string& subViewName)
    : ISubView(subViewName)
{
    m_currentRoot = std::filesystem::current_path();
}

// 内部绘制逻辑 (Clay/ImGui)
void FileManagerView::onUpdate(LayoutContext& layoutContext,
                               UIManager*     sourceManager)
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    auto& skinCfg = Config::SkinManager::instance();

    ImFont* fileManagerFont = skinCfg.getFont("filemanager");
    if ( fileManagerFont ) ImGui::PushFont(fileManagerFont);

    CLayVBox rootVBox;

    if ( !project ) {
        // --- 尚未打开项目时的界面 ---
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
                    ImVec2 textSize =
                        ImGui::CalcTextSize(TR("ui.file_manager.initial_hint"));
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                         (r.width - textSize.x) * 0.5f);
                    ImGui::TextEx(TR("ui.file_manager.initial_hint"));
                })
            .addSpring();

        CLayHBox buttonHBox;
        buttonHBox.addSpring()
            .addElement(
                "OpenDirButton",
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

            recentVBox.addElement(
                "RecentTitle",
                Sizing::Grow(),
                Sizing::Fixed(20),
                [](Clay_BoundingBox r, bool isHovered) {
                    ImGui::TextDisabled("%s", TR("ui.file.open_recent").data());
                });

            for ( size_t i = 0; i < recent.size(); ++i ) {
                const auto& path = recent[i];
                std::string id   = fmt::format("RecentItem_{}", i);

                recentVBox.addElement(
                    id,
                    Sizing::Grow(),
                    Sizing::Fixed(20),
                    [path, &skinCfg](Clay_BoundingBox r, bool isHovered) {
                        std::filesystem::path p(path);
                        std::string           name = p.filename().string();
                        if ( name.empty() ) name = path;

                        // 使用 Skin 中的 icon 颜色
                        auto   iconCol = skinCfg.getColor("icon");
                        ImVec4 col(iconCol.r, iconCol.g, iconCol.b, iconCol.a);

                        if ( isHovered ) {
                            col = ImGui::GetStyle()
                                      .Colors[ImGuiCol_ButtonHovered];
                        }

                        ImGui::PushStyleColor(ImGuiCol_Text, col);
                        ImGui::TextUnformatted(name.c_str());
                        ImGui::PopStyleColor();

                        ImVec2 min = ImGui::GetItemRectMin();
                        ImVec2 max = ImGui::GetItemRectMax();
                        min.y      = max.y;
                        ImGui::GetWindowDrawList()->AddLine(
                            min,
                            max,
                            ImGui::ColorConvertFloat4ToU32(col),
                            1.0f);

                        if ( isHovered ) {
                            ImGui::SetTooltip("%s", path.c_str());
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        }

                        if ( ImGui::IsItemClicked() ) {
                            Event::OpenProjectEvent ev;
                            ev.m_projectPath = path;
                            Event::EventBus::instance().publish(ev);
                        }
                    });
            }
        }

        rootVBox.setPadding(12, 12, 12, 12)
            .setSpacing(12)
            .addLayout(
                "labelHBox", labelHBox, Sizing::Grow(), Sizing::Fixed(40))
            .addLayout(
                "buttonHBox", buttonHBox, Sizing::Grow(), Sizing::Fixed(40))
            .addLayout(
                "recentVBox", recentVBox, Sizing::Grow(), Sizing::Grow());

        rootVBox.addSpring();
        rootVBox.render(layoutContext);
    } else {
        // --- 已打开项目时的界面 ---
        m_currentRoot = project->m_projectRoot;

        CLayHBox titleHBox;
        titleHBox.addElement(
            "ProjectTitle",
            Sizing::Grow(),
            Sizing::Fixed(ImGui::GetFrameHeight()),
            [project](Clay_BoundingBox r, bool isHovered) {
                ImGui::TextColored(
                    { 0.4f, 0.8f, 1.0f, 1.0f },
                    "Root: %s",
                    project->m_projectRoot.filename().string().c_str());
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip("%s",
                                      project->m_projectRoot.string().c_str());
                }
            });

        CLayVBox treeVBox;
        treeVBox.setPadding(4, 4, 4, 4)
            .addElement("FileTree",
                        Sizing::Grow(),
                        Sizing::Grow(),
                        [this](Clay_BoundingBox r, bool isHovered) {
                            ImGui::BeginChild("FileTreeChild");
                            float indent = ImGui::CalcTextSize("AA").x;
                            ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing,
                                                indent);
                            this->drawDirectoryRecursive(m_currentRoot);
                            ImGui::PopStyleVar();
                            ImGui::EndChild();
                        });

        rootVBox.setPadding(8, 8, 8, 8)
            .setSpacing(4)
            .addLayout(
                "titleHBox", titleHBox, Sizing::Grow(), Sizing::Fixed(24))
            .addLayout("treeVBox", treeVBox, Sizing::Grow(), Sizing::Grow());

        rootVBox.render(layoutContext);
    }

    if ( fileManagerFont ) ImGui::PopFont();
}

void FileManagerView::drawDirectoryRecursive(const std::filesystem::path& path)
{
    try {
        for ( const auto& entry : std::filesystem::directory_iterator(path) ) {
            const auto& p        = entry.path();
            std::string filename = p.filename().string();

            if ( filename.size() > 1 && filename[0] == '.' ) continue;

            if ( entry.is_directory() ) {
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                           ImGuiTreeNodeFlags_OpenOnDoubleClick;
                bool open = ImGui::TreeNodeEx(filename.c_str(), flags);
                if ( open ) {
                    drawDirectoryRecursive(p);
                    ImGui::TreePop();
                }
            } else {
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf |
                                           ImGuiTreeNodeFlags_NoTreePushOnOpen;
                ImGui::TreeNodeEx(filename.c_str(), flags);
                if ( ImGui::IsItemClicked() ) {
                    XINFO("Clicked file: {}", p.string());
                }
            }
        }
    } catch ( ... ) {
    }
}

void FileManagerView::openFolderPicker()
{
    auto& config = Config::AppConfig::instance().getEditorSettings();
    XINFO("Opening folder picker, style: {}",
          config.filePickerStyle == Config::FilePickerStyle::Native
              ? "Native"
              : "Unified");

    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        // --- 使用 nativefiledialog-extended (系统原生) ---
        nfdu8char_t* outPath = nullptr;
        nfdresult_t  result  = NFD_PickFolder(&outPath, nullptr);

        if ( result == NFD_OKAY ) {
            Event::OpenProjectEvent ev;
            ev.m_projectPath = outPath;
            Event::EventBus::instance().publish(ev);
            NFD_FreePath(outPath);
        } else if ( result == NFD_CANCEL ) {
            XINFO("User cancelled native folder picker.");
        } else {
            XERROR("NFD Error: {}", NFD_GetError());
        }
    } else {
        // --- 使用 ImGuiFileDialog (统一风格) ---
        IGFD::FileDialogConfig fdConfig;
        fdConfig.path              = ".";
        fdConfig.countSelectionMax = 1;
        fdConfig.flags             = ImGuiFileDialogFlags_Default;
        // ImGuiFileDialog 的特定标志位或设置可以在这里配置
        ImGuiFileDialog::Instance()->OpenDialog(
            "ProjectFolderPicker",
            TR("ui.file_manager.open_directory"),
            nullptr,  // 为空且不带过滤器通常会被识别为目录选择 (依赖版本配置)
            fdConfig);
    }
}

}  // namespace MMM::UI
