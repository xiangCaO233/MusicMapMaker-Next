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

        rootVBox.setPadding(12, 12, 12, 12)
            .setSpacing(12)
            .addLayout(
                "labelHBox", labelHBox, Sizing::Grow(), Sizing::Fixed(40))
            .addLayout(
                "buttonHBox", buttonHBox, Sizing::Grow(), Sizing::Fixed(40));

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
                            this->drawDirectoryRecursive(m_currentRoot);
                            ImGui::EndChild();
                        });

        rootVBox.setPadding(8, 8, 8, 8)
            .setSpacing(4)
            .addLayout(
                "titleHBox", titleHBox, Sizing::Grow(), Sizing::Fixed(24))
            .addLayout("treeVBox", treeVBox, Sizing::Grow(), Sizing::Grow());

        rootVBox.render(layoutContext);
    }

    // 处理 ImGuiFileDialog 的显示 (如果是 Unified 模式且已开启)
    auto& editorSettings = engine.getEditorConfig().settings;
    if ( editorSettings.filePickerStyle == Config::FilePickerStyle::Unified &&
         ImGuiFileDialog::Instance()->Display("ProjectFolderPicker",
                                              ImGuiWindowFlags_NoCollapse,
                                              { 600, 400 }) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            std::string folderPath =
                ImGuiFileDialog::Instance()->GetFilePathName();
            // 如果选择的是文件夹且没有点击具体项，GetFilePathName 可能为空
            if ( folderPath.empty() ) {
                folderPath = ImGuiFileDialog::Instance()->GetCurrentPath();
            }
            Event::OpenProjectEvent ev;
            ev.m_projectPath = folderPath;
            Event::EventBus::instance().publish(ev);
        }
        ImGuiFileDialog::Instance()->Close();
    }
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
        XINFO("原生文件选择器");
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
        XINFO("统一文件选择器 (模态)");
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
