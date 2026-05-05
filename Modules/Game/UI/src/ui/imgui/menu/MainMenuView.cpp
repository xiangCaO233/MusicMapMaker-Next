#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/imgui/menu/MainMenuView.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/UISettingsTabEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "network/UpdateChecker.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include <ImGuiFileDialog.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nfd.h>

namespace MMM::UI
{

MainMenuView::MainMenuView()
    : m_openFileMenuNextFrame(false)
    , m_openEditMenuNextFrame(false)
    , m_openHelpMenuNextFrame(false)
    , m_closeFileMenuNextFrame(false)
    , m_closeEditMenuNextFrame(false)
    , m_closeHelpMenuNextFrame(false)
    , m_showAboutPopup(false)
    , m_showUpdatePopup(false)
    , m_showCheckingPopup(false)
    , m_updateChecker(std::make_unique<MMM::Network::UpdateChecker>())
{
}

MainMenuView::~MainMenuView() {}

void MainMenuView::dispatchCommand(const MMM::Logic::LogicCommand& cmd)
{
    Event::EventBus::instance().publish(Event::LogicCommandEvent(cmd));
}

void MainMenuView::handleHotkeys(UIManager* sourceManager)
{
    auto* project    = Logic::EditorEngine::instance().getCurrentProject();
    bool  hasProject = (project != nullptr);

    ImGuiIO& io = ImGui::GetIO();
    // 只有在没有文本输入激活时才处理快捷键，除非是 Ctrl 组合键
    if ( ImGui::IsAnyItemActive() && !io.KeyCtrl ) return;

    if ( io.KeyCtrl ) {
        if ( ImGui::IsKeyPressed(ImGuiKey_N) ) {
            if ( io.KeyShift ) {
                // New project logic placeholder
            } else if ( hasProject ) {
                auto* wizard = sourceManager->getView<NewBeatmapWizard>(
                    "NewBeatmapWizard");
                if ( wizard ) wizard->open();
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_O) ) {
            openFolderPicker();
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_S) ) {
            if ( io.KeyShift ) {
                openExportFilePicker("");
            } else {
                dispatchCommand(Logic::CmdSaveBeatmap{});
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_Z) ) {
            if ( io.KeyShift ) {
                dispatchCommand(Logic::CmdRedo{});
            } else {
                dispatchCommand(Logic::CmdUndo{});
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_Y) ) {
            dispatchCommand(Logic::CmdRedo{});
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_C, false) ) {
            dispatchCommand(Logic::CmdCopy{});
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_V, false) ) {
            dispatchCommand(Logic::CmdPaste{});
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_X, false) ) {
            dispatchCommand(Logic::CmdCut{});
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_A, false) ) {
            dispatchCommand(Logic::CmdSelectAll{});
        }
    } else if ( io.KeyAlt ) {
        if ( ImGui::IsKeyPressed(ImGuiKey_F, false) ) {
            if ( ImGui::IsPopupOpen(TR("ui.file")) ) {
                m_closeFileMenuNextFrame = true;
            } else {
                m_openFileMenuNextFrame = true;
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_E, false) ) {
            if ( ImGui::IsPopupOpen(TR("ui.edit")) ) {
                m_closeEditMenuNextFrame = true;
            } else {
                m_openEditMenuNextFrame = true;
            }
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_H, false) ) {
            if ( ImGui::IsPopupOpen(TR("ui.help")) ) {
                m_closeHelpMenuNextFrame = true;
            } else {
                m_openHelpMenuNextFrame = true;
            }
        }
    } else if ( !io.KeySuper && !io.KeyShift ) {
        if ( ImGui::IsKeyPressed(ImGuiKey_Space, false) ) {
            bool playing = Logic::EditorEngine::instance().isPlaybackPlaying();
            dispatchCommand(Logic::CmdSetPlayState{ !playing });
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
        fdConfig.path              = config.lastFilePickerPath;
        fdConfig.countSelectionMax = 1;
        fdConfig.fileName          = "map.osz";
        fdConfig.flags             = ImGuiFileDialogFlags_Default;
        ImGuiFileDialog::Instance()->OpenDialog(
            "PackFilePicker", TR("ui.file.pack"), ".osz,.mcz,.zip", fdConfig);
    }
}

void MainMenuView::openExportFilePicker(const std::string& ext)
{
    auto& config = Config::AppConfig::instance().getEditorSettings();

    std::string defaultName = "map" + (ext.empty() ? ".mmm" : ext);
    auto        session = Logic::EditorEngine::instance().getActiveSession();
    if ( session && session->getContext().currentBeatmap ) {
        auto& meta = session->getContext().currentBeatmap->m_baseMapMetadata;
        if ( ext == ".imd" ) {
            defaultName = fmt::format(
                "{}_{}k_{}.imd", meta.title, meta.track_count, meta.version);
        } else {
            defaultName = meta.name + (ext.empty() ? ".mmm" : ext);
        }
    }

    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        nfdu8char_t*      outPath = nullptr;
        nfdu8filteritem_t filters[4];
        int               filterCount = 0;

        if ( ext == ".mmm" || ext == "" ) {
            filters[filterCount++] = { "MusicMapMaker Beatmap", "mmm" };
        }
        if ( ext == ".osu" || ext == "" ) {
            filters[filterCount++] = { "osu!mania Beatmap", "osu" };
        }
        if ( ext == ".imd" || ext == "" ) {
            filters[filterCount++] = { "IvoryMusicData", "imd" };
        }
        if ( ext == ".mc" || ext == "" ) {
            filters[filterCount++] = { "Malody Chart", "mc" };
        }

        nfdresult_t result = NFD_SaveDialogU8(
            &outPath, filters, filterCount, nullptr, defaultName.c_str());

        if ( result == NFD_OKAY ) {
            dispatchCommand(Logic::CmdSaveBeatmapAs{ outPath });
            NFD_FreePath(outPath);
        }
    } else {
        IGFD::FileDialogConfig fdConfig;
        fdConfig.path              = config.lastFilePickerPath;
        fdConfig.countSelectionMax = 1;
        fdConfig.fileName          = defaultName;
        fdConfig.flags             = ImGuiFileDialogFlags_Default;

        std::string filterStr;
        if ( ext == ".mmm" )
            filterStr = ".mmm";
        else if ( ext == ".osu" )
            filterStr = ".osu";
        else if ( ext == ".imd" )
            filterStr = ".imd";
        else if ( ext == ".mc" )
            filterStr = ".mc";
        else
            filterStr = ".mmm,.osu,.imd,.mc";

        ImGuiFileDialog::Instance()->OpenDialog("SaveAsFilePicker",
                                                TR("ui.file.save_as"),
                                                filterStr.c_str(),
                                                fdConfig);
    }
}

void MainMenuView::startUpdateCheck()
{
    m_showCheckingPopup = true;
    m_updateChecker->checkAsync();
}

void MainMenuView::renderHelpMenu(UIManager* sourceManager)
{
    auto MenuItemWithFontIcon = [](const char* icon,
                                   const char* label,
                                   const char* shortcut = nullptr,
                                   bool        enabled  = true) -> bool {
        ImVec4 iconVec4 = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

        float gap = ImGui::CalcTextSize(" ").x * 0.5f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(gap, 0));

        const char* iconPtr = icon ? icon : "  ";

        bool clicked =
            ImGui::MenuItemEx(label, iconPtr, shortcut, false, enabled);

        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return clicked;
    };

    if ( m_openHelpMenuNextFrame ) {
        ImGui::OpenPopup(TR("ui.help"));
        m_openHelpMenuNextFrame = false;
    }
    if ( ImGui::BeginMenu(TR("ui.help")) ) {
        if ( m_closeHelpMenuNextFrame ) {
            ImGui::CloseCurrentPopup();
            m_closeHelpMenuNextFrame = false;
        }

        if ( MenuItemWithFontIcon(ICON_MMM_DOWNLOAD,
                                  TR("ui.help.check_update")) ) {
            startUpdateCheck();
        }
        if ( MenuItemWithFontIcon(ICON_MMM_INFO_CIRCLE, TR("ui.help.about")) ) {
            m_showAboutPopup = true;
        }
        ImGui::EndMenu();
    }
}

void MainMenuView::renderAboutPopup()
{
    if ( m_showAboutPopup ) {
        ImGui::OpenPopup(TR("ui.help.about_title"));
        m_showAboutPopup = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 280), ImGuiCond_Appearing);

    if ( ImGui::BeginPopupModal(
             TR("ui.help.about_title"),
             nullptr,
             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize) ) {
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();

        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16 * dpiScale);

        // 标题
        ImFont* contentFont =
            Config::SkinManager::instance().getFont("content");
        if ( contentFont ) ImGui::PushFont(contentFont);

        ImGui::TextUnformatted(TR("ui.help.app_name").data());

        if ( contentFont ) ImGui::PopFont();

        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16 * dpiScale);

        ImVec4 dimColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);

        // 版本号
        ImGui::TextUnformatted(TR("ui.help.current_version").data());
        ImGui::SameLine();
        ImGui::TextColored(dimColor, APP_VERSION);

        // 构建配置
        ImGui::TextUnformatted(TR("ui.help.build_type").data());
        ImGui::SameLine();
#if BUILD_TYPE_DEBUG
        ImGui::TextColored(dimColor, "Debug");
#else
        ImGui::TextColored(dimColor, "Release");
#endif

        // 运行平台
        ImGui::TextUnformatted(TR("ui.help.platform").data());
        ImGui::SameLine();
        ImGui::TextColored(dimColor, APP_PLATFORM);

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 16 * dpiScale);

        ImGui::TextColored(dimColor,
                           "Copyright (C) 2025 xiang233. All rights reserved.");

        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f -
                             ImGui::CalcTextSize(TR("ui.help.ok").data()).x *
                                 0.5f);

        if ( ImGui::Button(TR("ui.help.ok").data(),
                           ImVec2(100 * dpiScale, 0)) ) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void MainMenuView::renderUpdateCheckingPopup()
{
    if ( m_showCheckingPopup ) {
        ImGui::OpenPopup(TR("ui.help.check_update"));
        m_showCheckingPopup = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    bool open = true;
    if ( ImGui::BeginPopupModal(
             TR("ui.help.check_update"),
             &open,
             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize) ) {
        auto  info     = m_updateChecker->getInfo();
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();

        ImGui::Spacing();

        if ( info.status == MMM::Network::UpdateStatus::kChecking ) {
            ImGui::SetCursorPosX(
                ImGui::GetWindowWidth() * 0.5f -
                ImGui::CalcTextSize(TR("ui.help.checking").data()).x * 0.5f);
            ImGui::TextUnformatted(TR("ui.help.checking").data());

            ImGui::Spacing();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f -
                                 40.0f * dpiScale);
            ImGui::SetNextItemWidth(80.0f * dpiScale);
            float fraction = -1.0f * (float)ImGui::GetTime();
            ImGui::ProgressBar(fraction, ImVec2(80.0f * dpiScale, 0.0f));
        } else if ( info.status == MMM::Network::UpdateStatus::kUpToDate ) {
            ImVec2 textSize =
                ImGui::CalcTextSize(TR("ui.help.up_to_date").data());
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f -
                                 textSize.x * 0.5f);
            ImGui::TextUnformatted(TR("ui.help.up_to_date").data());

            ImGui::Spacing();
            ImGui::SetCursorPosX(
                ImGui::GetWindowWidth() * 0.5f -
                ImGui::CalcTextSize(TR("ui.help.ok").data()).x * 0.5f);
            if ( ImGui::Button(TR("ui.help.ok").data(),
                               ImVec2(100 * dpiScale, 0)) ) {
                ImGui::CloseCurrentPopup();
            }
        } else if ( info.status == MMM::Network::UpdateStatus::kUpdateFound ) {
            ImGui::CloseCurrentPopup();
            m_showUpdatePopup = true;
        } else if ( info.status == MMM::Network::UpdateStatus::kError ) {
            ImVec4 errColor(1.0f, 0.3f, 0.3f, 1.0f);
            ImVec2 textSize =
                ImGui::CalcTextSize(TR("ui.help.update_error").data());
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f -
                                 textSize.x * 0.5f);
            ImGui::TextColored(
                errColor, "%s", TR("ui.help.update_error").data());

            ImGui::Spacing();
            ImGui::TextWrapped("%s", info.errorMessage.c_str());

            ImGui::Spacing();
            ImGui::SetCursorPosX(
                ImGui::GetWindowWidth() * 0.5f -
                ImGui::CalcTextSize(TR("ui.help.ok").data()).x * 0.5f);
            if ( ImGui::Button(TR("ui.help.ok").data(),
                               ImVec2(100 * dpiScale, 0)) ) {
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::EndPopup();
    }

    // 如果 update is found, open the update popup next frame
    if ( m_showUpdatePopup ) {
        m_showCheckingPopup = false;
    }
}

void MainMenuView::renderUpdatePopup()
{
    if ( m_showUpdatePopup ) {
        ImGui::OpenPopup(TR("ui.help.update_found"));
        m_showUpdatePopup = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if ( ImGui::BeginPopupModal(
             TR("ui.help.update_found"),
             nullptr,
             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize) ) {
        auto  info     = m_updateChecker->getInfo();
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();

        ImGui::Spacing();

        // 版本比较
        ImGui::TextUnformatted(TR("ui.help.current_version").data());
        ImGui::SameLine();
        ImGui::TextUnformatted(info.currentVersion.c_str());

        ImVec4 newColor(0.3f, 1.0f, 0.3f, 1.0f);
        ImGui::TextUnformatted(TR("ui.help.latest_version").data());
        ImGui::SameLine();
        ImGui::TextColored(newColor, "%s", info.latestVersion.c_str());

        if ( !info.releaseDate.empty() ) {
            ImGui::TextUnformatted(TR("ui.help.release_date").data());
            ImGui::SameLine();
            ImGui::TextUnformatted(info.releaseDate.c_str());
        }

        // 更新内容
        if ( !info.changelog.empty() ) {
            ImGui::Spacing();
            ImGui::TextUnformatted(TR("ui.help.changelog").data());
            ImGui::Spacing();

            ImVec2 regionAvail = ImGui::GetContentRegionAvail();
            float  textHeight  = std::min(150.0f * dpiScale, regionAvail.y);
            ImGui::BeginChild(
                "ChangelogScroll",
                ImVec2(ImGui::GetContentRegionAvail().x, textHeight),
                ImGuiChildFlags_Borders);
            ImGui::TextWrapped("%s", info.changelog.c_str());
            ImGui::EndChild();
        }

        ImGui::Spacing();
        ImGui::Spacing();

        float buttonWidth  = 120 * dpiScale;
        float totalButtons = info.downloadUrl.empty() ? 1 : 2;
        float offsetX =
            (ImGui::GetWindowWidth() -
             (totalButtons * buttonWidth +
              (totalButtons - 1) * ImGui::GetStyle().ItemSpacing.x)) *
            0.5f;
        ImGui::SetCursorPosX(offsetX);

        if ( !info.downloadUrl.empty() ) {
            if ( ImGui::Button(TR("ui.help.download").data(),
                               ImVec2(buttonWidth, 0)) ) {
                MMM::Network::UpdateChecker::openUrlInBrowser(info.downloadUrl);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
        }

        if ( ImGui::Button(TR("ui.help.cancel").data(),
                           ImVec2(buttonWidth, 0)) ) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void MainMenuView::update(UIManager* sourceManager)
{
    renderMenus(sourceManager);
    renderInfoText();
}

void MainMenuView::renderMenus(UIManager* sourceManager)
{
    handleHotkeys(sourceManager);

    Config::SkinManager& skinCfg = Config::SkinManager::instance();

    float dpiScale = MMM::Config::AppConfig::instance().getWindowContentScale();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(8.0f * dpiScale, 8.0f * dpiScale));
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(6.0f * dpiScale, ImGui::GetStyle().FramePadding.y));

    auto MenuItemWithFontIcon = [](const char* icon,
                                   const char* label,
                                   const char* shortcut = nullptr,
                                   bool        enabled  = true) -> bool {
        ImVec4 iconVec4 = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, iconVec4);

        float gap = ImGui::CalcTextSize(" ").x * 0.5f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(gap, 0));

        const char* iconPtr = icon ? icon : "  ";

        bool clicked =
            ImGui::MenuItemEx(label, iconPtr, shortcut, false, enabled);

        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return clicked;
    };

    ImFont* menuFont = skinCfg.getFont("menu");
    if ( menuFont ) ImGui::PushFont(menuFont);

    // ========== File Menu ==========
    if ( m_openFileMenuNextFrame ) {
        ImGui::OpenPopup(TR("ui.file"));
        m_openFileMenuNextFrame = false;
    }
    if ( ImGui::BeginMenu(TR("ui.file")) ) {
        if ( m_closeFileMenuNextFrame ) {
            ImGui::CloseCurrentPopup();
            m_closeFileMenuNextFrame = false;
        }

        auto* project    = Logic::EditorEngine::instance().getCurrentProject();
        bool  hasProject = (project != nullptr);

        if ( MenuItemWithFontIcon(
                 ICON_MMM_BOOK, TR("ui.file.new_pro"), "Ctrl+Shift+N") ) {}
        if ( MenuItemWithFontIcon(
                 ICON_MMM_FILE, TR("ui.file.new_map"), "Ctrl+N", hasProject) ) {
            auto* wizard =
                sourceManager->getView<NewBeatmapWizard>("NewBeatmapWizard");
            if ( wizard ) wizard->open();
        }
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
                    std::filesystem::path p(
                        reinterpret_cast<const char8_t*>(path.c_str()));
                    auto        u8name = p.filename().u8string();
                    std::string name(
                        reinterpret_cast<const char*>(u8name.c_str()),
                        u8name.size());
                    if ( ImGui::MenuItem(name.c_str(), path.c_str()) ) {
                        Event::OpenProjectEvent ev;
                        ev.m_projectPath = p;
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
        if ( MenuItemWithFontIcon(
                 nullptr, TR("ui.file.save_as"), "Ctrl+Shift+S") ) {
            openExportFilePicker("");
        }

        if ( MenuItemWithFontIcon(ICON_MMM_PACK, TR("ui.file.pack")) ) {
            openPackFilePicker();
        }
        ImGui::EndMenu();
    }

    // ========== Edit Menu ==========
    if ( m_openEditMenuNextFrame ) {
        ImGui::OpenPopup(TR("ui.edit"));
        m_openEditMenuNextFrame = false;
    }
    if ( ImGui::BeginMenu(TR("ui.edit")) ) {
        if ( m_closeEditMenuNextFrame ) {
            ImGui::CloseCurrentPopup();
            m_closeEditMenuNextFrame = false;
        }
        if ( MenuItemWithFontIcon(
                 ICON_MMM_UNDO, TR("ui.edit.undo"), "Ctrl+Z") ) {
            dispatchCommand(Logic::CmdUndo{});
        }
        if ( MenuItemWithFontIcon(
                 ICON_MMM_REDO, TR("ui.edit.redo"), "Ctrl+Y / Ctrl+Shift+Z") ) {
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
                 nullptr, TR("ui.edit.select_all"), "Ctrl+A") ) {
            dispatchCommand(Logic::CmdSelectAll{});
        }
        ImGui::Separator();
        bool playing      = Logic::EditorEngine::instance().isPlaybackPlaying();
        const char* pIcon = playing ? ICON_MMM_PAUSE : ICON_MMM_PLAY;
        if ( MenuItemWithFontIcon(pIcon, TR("ui.edit.play_pause"), "Space") ) {
            dispatchCommand(Logic::CmdSetPlayState{ !playing });
        }
        ImGui::Separator();
        if ( MenuItemWithFontIcon(ICON_MMM_FILE,
                                  TR("ui.edit.beatmap_settings")) ) {
            Event::UISubViewToggleEvent evt;
            evt.targetFloatManagerName = "SideBarManager";
            evt.subViewId              = TR("title.settings_manager").data();
            evt.showSubView            = true;
            Event::EventBus::instance().publish(evt);

            Event::UISettingsTabEvent tabEvt;
            tabEvt.tab = Event::SettingsTab::Beatmap;
            Event::EventBus::instance().publish(tabEvt);
        }
        ImGui::EndMenu();
    }

    // ========== Help Menu ==========
    renderHelpMenu(sourceManager);

    // ========== Popups ==========
    renderAboutPopup();
    renderUpdateCheckingPopup();
    renderUpdatePopup();

    if ( menuFont ) ImGui::PopFont();
    ImGui::PopStyleVar(2);  // Pop WindowPadding and FramePadding
}

void MainMenuView::renderInfoText()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("MusicMapMaker(Gamma)");
    ImGui::SameLine();
    ImGui::Text(
        "%.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
}

}  // namespace MMM::UI
