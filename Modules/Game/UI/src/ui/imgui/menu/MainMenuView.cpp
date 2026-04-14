#include "ui/imgui/menu/MainMenuView.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "log/colorful-log.h"
#include "ui/Icons.h"
#include <ImGuiFileDialog.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nfd.h>

namespace MMM::UI
{

MainMenuView::MainMenuView() {}

MainMenuView::~MainMenuView() {}

void MainMenuView::dispatchCommand(const MMM::Logic::LogicCommand& cmd)
{
    Event::EventBus::instance().publish(Event::LogicCommandEvent(cmd));
}

void MainMenuView::handleHotkeys()
{
    ImGuiIO& io = ImGui::GetIO();
    // 只有在没有文本输入激活时才处理快捷键，除非是 Ctrl 组合键
    if ( ImGui::IsAnyItemActive() && !io.KeyCtrl ) return;

    if ( io.KeyCtrl ) {
        if ( ImGui::IsKeyPressed(ImGuiKey_N) ) {
            // New project logic placeholder
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_O) ) {
            openFolderPicker();
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_S) ) {
            dispatchCommand(Logic::CmdSaveBeatmap{});
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_Z) ) {
            dispatchCommand(Logic::CmdUndo{});
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_Y) ) {
            dispatchCommand(Logic::CmdRedo{});
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_C) ) {
            dispatchCommand(Logic::CmdCopy{});
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_V) ) {
            dispatchCommand(Logic::CmdPaste{});
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_X) ) {
            dispatchCommand(Logic::CmdCut{});
        }
    } else {
        if ( ImGui::IsKeyPressed(ImGuiKey_Space) ) {
            // Toggle play state placeholder
        }
    }
}

void MainMenuView::openFolderPicker()
{
    auto& config = Config::AppConfig::instance().getEditorSettings();
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        nfdu8char_t* outPath = nullptr;
        nfdresult_t  result  = NFD_PickFolder(&outPath, nullptr);

        if ( result == NFD_OKAY ) {
            Event::OpenProjectEvent ev;
            ev.m_projectPath = outPath;
            Event::EventBus::instance().publish(ev);
            NFD_FreePath(outPath);
        }
    } else {
        IGFD::FileDialogConfig fdConfig;
        fdConfig.path              = ".";
        fdConfig.countSelectionMax = 1;
        fdConfig.flags             = ImGuiFileDialogFlags_Default;
        ImGuiFileDialog::Instance()->OpenDialog(
            "ProjectFolderPicker",
            TR("ui.file_manager.open_directory"),
            nullptr,
            fdConfig);
    }
}

void MainMenuView::openPackFilePicker()
{
    auto& config = Config::AppConfig::instance().getEditorSettings();
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        nfdu8char_t*      outPath    = nullptr;
        nfdu8filteritem_t filters[1] = { { "Beatmap Package", "osz,mcz,zip" } };
        nfdresult_t       result =
            NFD_SaveDialogU8(&outPath, filters, 1, nullptr, "map.osz");

        if ( result == NFD_OKAY ) {
            dispatchCommand(Logic::CmdPackBeatmap{ outPath });
            NFD_FreePath(outPath);
        }
    } else {
        IGFD::FileDialogConfig fdConfig;
        fdConfig.path              = ".";
        fdConfig.countSelectionMax = 1;
        fdConfig.fileName          = "map.osz";
        fdConfig.flags             = ImGuiFileDialogFlags_Default;
        ImGuiFileDialog::Instance()->OpenDialog(
            "PackFilePicker", TR("ui.file.pack"), ".osz,.mcz,.zip", fdConfig);
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            // 这里获得了路径 filePath
            std::string filePath =
                ImGuiFileDialog::Instance()->GetFilePathName();
            Event::EventBus::instance().publish(
                Event::LogicCommandEvent(Logic::CmdPackBeatmap{ filePath }));
        }
    }
}

void MainMenuView::openExportFilePicker(const std::string& ext)
{
    auto& config = Config::AppConfig::instance().getEditorSettings();
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        nfdu8char_t*      outPath = nullptr;
        nfdu8filteritem_t filters[2];
        int               filterCount = 0;

        if ( ext == ".osu" || ext == "" ) {
            filters[filterCount++] = { "osu!mania Beatmap", "osu" };
        }
        if ( ext == ".imd" || ext == "" ) {
            filters[filterCount++] = { "IvoryMusicData", "imd" };
        }

        std::string defaultName = "map" + (ext.empty() ? ".osu" : ext);
        nfdresult_t result      = NFD_SaveDialogU8(
            &outPath, filters, filterCount, nullptr, defaultName.c_str());

        if ( result == NFD_OKAY ) {
            dispatchCommand(Logic::CmdSaveBeatmapAs{ outPath });
            NFD_FreePath(outPath);
        }
    } else {
        IGFD::FileDialogConfig fdConfig;
        fdConfig.path              = ".";
        fdConfig.countSelectionMax = 1;
        fdConfig.fileName          = "map" + (ext.empty() ? ".osu" : ext);
        fdConfig.flags             = ImGuiFileDialogFlags_Default;

        std::string filterStr;
        if ( ext == ".osu" )
            filterStr = ".osu";
        else if ( ext == ".imd" )
            filterStr = ".imd";
        else
            filterStr = ".osu,.imd";

        ImGuiFileDialog::Instance()->OpenDialog("ExportFilePicker",
                                                TR("ui.file.export"),
                                                filterStr.c_str(),
                                                fdConfig);

        // 注意：IGFD 需要在后续 update 中轮询 IsOk()。这里仅触发打开。
        // 为了保持逻辑简单，我们目前主要依赖 Native 对话框或是在 update
        // 里轮询。 由于 MainMenuView::update 每一帧都在跑，我们可以检查。
    }
}

void MainMenuView::update()
{
    handleHotkeys();

    // 检查文件对话框结果 (针对非 Native 模式)
    if ( ImGuiFileDialog::Instance()->Display("ExportFilePicker",
                                              ImGuiWindowFlags_NoCollapse,
                                              ImVec2(600, 400)) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            std::string filePath =
                ImGuiFileDialog::Instance()->GetFilePathName();
            dispatchCommand(Logic::CmdSaveBeatmapAs{ filePath });
        }
        ImGuiFileDialog::Instance()->Close();
    }

    Config::SkinManager& skinCfg = Config::SkinManager::instance();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float                dpiScale = viewport->DpiScale;

    // 增加全局边距，确保图标不被截断，并提供更好的视觉空间
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(8.0f * dpiScale, 8.0f * dpiScale));
    // 保持已有的垂直 Padding（由外部 Host 窗口决定），仅增加水平边距
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(6.0f * dpiScale, ImGui::GetStyle().FramePadding.y));

    auto MenuItemWithFontIcon = [&skinCfg](const char* icon,
                                           const char* label,
                                           const char* shortcut = nullptr,
                                           bool        enabled = true) -> bool {
        Config::Color iconColor = skinCfg.getColor("icon");
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            ImVec4(iconColor.r, iconColor.g, iconColor.b, iconColor.a));

        // 图标与文本间隔设为半个空格宽
        float gap = ImGui::CalcTextSize(" ").x * 0.5f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(gap, 0));

        // 统一占位符，确保无图标项的文本对齐
        // 这里使用两个空格作为图标列的基准占位
        const char* iconPtr = icon ? icon : "  ";

        bool clicked =
            ImGui::MenuItemEx(label, iconPtr, shortcut, false, enabled);

        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return clicked;
    };

    ImFont* menuFont = skinCfg.getFont("menu");
    if ( menuFont ) ImGui::PushFont(menuFont);

    if ( ImGui::BeginMenu(TR("ui.file")) ) {
        if ( MenuItemWithFontIcon(
                 ICON_MMM_BOOK, TR("ui.file.new_pro"), "Ctrl+N") ) {}
        if ( MenuItemWithFontIcon(ICON_MMM_FILE, TR("ui.file.new_map")) ) {}
        ImGui::Separator();

        if ( MenuItemWithFontIcon(
                 ICON_MMM_FOLDER_OPEN, TR("ui.file.open_pro"), "Ctrl+O") ) {
            openFolderPicker();
        }

        if ( ImGui::BeginMenu(TR("ui.file.open_recent")) ) {
            const auto& recent =
                Config::AppConfig::instance().getEditorConfig().recentProjects;
            if ( recent.empty() ) {
                ImGui::MenuItem(TR("ui.file.no_recent"), nullptr, false, false);
            } else {
                for ( const auto& path : recent ) {
                    std::filesystem::path p(path);
                    std::string           name = p.filename().string();
                    if ( ImGui::MenuItem(name.c_str(), path.c_str()) ) {
                        Event::OpenProjectEvent ev;
                        ev.m_projectPath = path;
                        Event::EventBus::instance().publish(ev);
                    }
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();

        if ( MenuItemWithFontIcon(
                 ICON_MMM_SAVE, TR("ui.file.save"), "Ctrl+S") ) {
            dispatchCommand(Logic::CmdSaveBeatmap{});
        }
        if ( MenuItemWithFontIcon(nullptr, TR("ui.file.save_as")) ) {
            openExportFilePicker("");
        }

        ImGui::Separator();
        if ( ImGui::BeginMenu(TR("ui.file.export")) ) {
            if ( ImGui::MenuItem("osu!mania (.osu)") ) {
                openExportFilePicker(".osu");
            }
            if ( ImGui::MenuItem("IvoryMusicData (.imd)") ) {
                openExportFilePicker(".imd");
            }
            ImGui::EndMenu();
        }

        if ( MenuItemWithFontIcon(ICON_MMM_PACK, TR("ui.file.pack")) ) {
            openPackFilePicker();
        }
        ImGui::EndMenu();
    }

    if ( ImGui::BeginMenu(TR("ui.edit")) ) {
        if ( MenuItemWithFontIcon(
                 ICON_MMM_UNDO, TR("ui.edit.undo"), "Ctrl+Z") ) {
            dispatchCommand(Logic::CmdUndo{});
        }
        if ( MenuItemWithFontIcon(
                 ICON_MMM_REDO, TR("ui.edit.redo"), "Ctrl+Y") ) {
            dispatchCommand(Logic::CmdRedo{});
        }
        ImGui::Separator();
        if ( MenuItemWithFontIcon(
                 ICON_MMM_SCISSORS, TR("ui.edit.cut"), "Ctrl+X") ) {
            dispatchCommand(Logic::CmdCut{});
        }
        if ( MenuItemWithFontIcon(
                 ICON_MMM_COPY, TR("ui.edit.copy"), "Ctrl+C") ) {
            dispatchCommand(Logic::CmdCopy{});
        }
        if ( MenuItemWithFontIcon(nullptr, TR("ui.edit.paste"), "Ctrl+V") ) {
            dispatchCommand(Logic::CmdPaste{});
        }
        ImGui::Separator();
        if ( MenuItemWithFontIcon(
                 ICON_MMM_PLAY, TR("ui.edit.play_pause"), "Space") ) {}
        ImGui::EndMenu();
    }

    ImGuiIO& io = ImGui::GetIO();

    ImGui::Text("MusicMapMaker(Gamma)");
    ImGui::Text(
        "%.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);

    if ( menuFont ) ImGui::PopFont();
    ImGui::PopStyleVar(2);  // Pop WindowPadding and FramePadding
}

}  // namespace MMM::UI
