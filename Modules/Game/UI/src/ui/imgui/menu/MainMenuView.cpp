#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui/imgui/menu/MainMenuView.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/UISettingsTabEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmmversion.h"
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
        if ( ImGui::IsKeyPressed(ImGuiKey_M, false) ) {
            dispatchCommand(Logic::CmdMirrorSelected{});
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
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if ( ImGui::BeginPopupModal(
             TR("ui.help.about_title"),
             nullptr,
             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize) ) {
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(32.0f * dpiScale, 24.0f * dpiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(8.0f * dpiScale, 12.0f * dpiScale));

        // --- Logo & Title ---
        ImFont* titleFont = Config::SkinManager::instance().getFont("menu");
        if ( titleFont ) ImGui::PushFont(titleFont, 0.0f);

        std::string appLabel =
            std::string(ICON_MMM_MUSIC) + "  " + TR("ui.help.app_name").data();
        float titleWidth = ImGui::CalcTextSize(appLabel.c_str()).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - titleWidth) * 0.5f);
        ImGui::TextColored(
            ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "%s", appLabel.c_str());

        if ( titleFont ) ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Info Table ---
        if ( ImGui::BeginTable("AboutTable",
                               2,
                               ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_NoSavedSettings) ) {
            ImGui::TableSetupColumn(
                "L", ImGuiTableColumnFlags_WidthFixed, 140.0f * dpiScale);
            ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthStretch);

            auto AddRow = [&](const char* label, const char* value) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(value);
                ImGui::PopStyleColor();
            };

            AddRow(TR("ui.help.current_version").data(), MMM_VERSION_STRING);

#if BUILD_TYPE_DEBUG
            AddRow(TR("ui.help.build_type").data(), "Debug");
#else
            AddRow(TR("ui.help.build_type").data(), "Release");
#endif
            AddRow(TR("ui.help.platform").data(), MMM_PLATFORM);

            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Copyright ---
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const char* copyright =
            "Copyright (C) 2025 xiang233. All rights reserved.";
        float cpWidth = ImGui::CalcTextSize(copyright).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - cpWidth) * 0.5f);
        ImGui::TextUnformatted(copyright);
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // --- Button ---
        float btnWidth = 140.0f * dpiScale;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
        if ( ImGui::Button(TR("ui.help.ok").data(),
                           ImVec2(btnWidth, 36.0f * dpiScale)) ) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar(2);
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
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    bool open = true;
    if ( ImGui::BeginPopupModal(
             TR("ui.help.check_update"),
             &open,
             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize) ) {
        if ( !open ) ImGui::CloseCurrentPopup();
        auto  info     = m_updateChecker->getInfo();
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(32.0f * dpiScale, 24.0f * dpiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(8.0f * dpiScale, 16.0f * dpiScale));

        if ( info.status == MMM::Network::UpdateStatus::kChecking ) {
            float textWidth =
                ImGui::CalcTextSize(TR("ui.help.checking").data()).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
            ImGui::TextUnformatted(TR("ui.help.checking").data());

            float barWidth = 240.0f * dpiScale;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - barWidth) * 0.5f);
            float fraction = -1.0f * (float)ImGui::GetTime();
            ImGui::ProgressBar(
                fraction, ImVec2(barWidth, 12.0f * dpiScale), "");
        } else if ( info.status == MMM::Network::UpdateStatus::kUpToDate ) {
            float textWidth =
                ImGui::CalcTextSize(TR("ui.help.up_to_date").data()).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
            ImGui::TextUnformatted(TR("ui.help.up_to_date").data());

            float btnWidth = 120.0f * dpiScale;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
            if ( ImGui::Button(TR("ui.help.ok").data(),
                               ImVec2(btnWidth, 32.0f * dpiScale)) ) {
                ImGui::CloseCurrentPopup();
            }
        } else if ( info.status == MMM::Network::UpdateStatus::kUpdateFound ) {
            ImGui::CloseCurrentPopup();
            m_showUpdatePopup = true;
        } else if ( info.status == MMM::Network::UpdateStatus::kError ) {
            ImVec4 errColor(1.0f, 0.4f, 0.4f, 1.0f);
            float  textWidth =
                ImGui::CalcTextSize(TR("ui.help.update_error").data()).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
            ImGui::TextColored(
                errColor, "%s", TR("ui.help.update_error").data());

            ImGui::PushStyleColor(
                ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", info.errorMessage.c_str());
            ImGui::PopStyleColor();

            float btnWidth = 120.0f * dpiScale;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
            if ( ImGui::Button(TR("ui.help.ok").data(),
                               ImVec2(btnWidth, 32.0f * dpiScale)) ) {
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::PopStyleVar(2);
        ImGui::EndPopup();
    }

    if ( m_showUpdatePopup ) {
        m_showCheckingPopup = false;
    }
}

void MainMenuView::renderUpdatePopup()
{
    auto info = m_updateChecker->getInfo();

    if ( m_showUpdatePopup ) {
        ImGui::OpenPopup(TR("ui.help.update_found"));
        m_showUpdatePopup = false;
    } else if ( info.status == MMM::Network::UpdateStatus::kUpdateFound ) {
        if ( !ImGui::IsPopupOpen(TR("ui.help.update_found")) )
            ImGui::OpenPopup(TR("ui.help.update_found"));
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    bool isWorking = (info.status == MMM::Network::UpdateStatus::kDownloading ||
                      info.status == MMM::Network::UpdateStatus::kDownloaded);
    bool open      = true;
    if ( ImGui::BeginPopupModal(
             TR("ui.help.update_found"),
             isWorking ? nullptr : &open,
             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize) ) {
        if ( !open ) ImGui::CloseCurrentPopup();
        info           = m_updateChecker->getInfo();
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(32.0f * dpiScale, 24.0f * dpiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(8.0f * dpiScale, 16.0f * dpiScale));

        if ( info.status == MMM::Network::UpdateStatus::kUpdateFound ) {
            // --- Info Table ---
            if ( ImGui::BeginTable("UpdateInfoTable",
                                   2,
                                   ImGuiTableFlags_SizingFixedFit |
                                       ImGuiTableFlags_NoSavedSettings) ) {
                ImGui::TableSetupColumn(
                    "L", ImGuiTableColumnFlags_WidthFixed, 140.0f * dpiScale);
                ImGui::TableSetupColumn("R",
                                        ImGuiTableColumnFlags_WidthStretch);

                auto AddRow = [&](const char*   label,
                                  const char*   value,
                                  const ImVec4* color = nullptr) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(label);
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    if ( color )
                        ImGui::TextColored(*color, "%s", value);
                    else
                        ImGui::TextDisabled("%s", value);
                };

                AddRow(TR("ui.help.current_version").data(),
                       info.currentVersion.c_str());
                ImVec4 green(0.3f, 1.0f, 0.3f, 1.0f);
                AddRow(TR("ui.help.latest_version").data(),
                       info.latestVersion.c_str(),
                       &green);

                if ( !info.releaseDate.empty() )
                    AddRow(TR("ui.help.release_date").data(),
                           info.releaseDate.c_str());

                if ( info.downloadSize > 0 ) {
                    char sizeBuf[64];
                    if ( info.downloadSize >= 1024 * 1024 )
                        snprintf(sizeBuf,
                                 sizeof(sizeBuf),
                                 "%.1f MB",
                                 info.downloadSize / (1024.0 * 1024.0));
                    else if ( info.downloadSize >= 1024 )
                        snprintf(sizeBuf,
                                 sizeof(sizeBuf),
                                 "%.0f KB",
                                 info.downloadSize / 1024.0);
                    else
                        snprintf(sizeBuf,
                                 sizeof(sizeBuf),
                                 "%lld B",
                                 (long long)info.downloadSize);
                    AddRow(TR("ui.help.file_size").data(), sizeBuf);
                }
                ImGui::EndTable();
            }

            // --- Changelog ---
            if ( !info.changelog.empty() ) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextUnformatted(TR("ui.help.changelog").data());

                ImGui::BeginChild("ChangelogScroll",
                                  ImVec2(400.0f * dpiScale, 150.0f * dpiScale),
                                  ImGuiChildFlags_Borders);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                                    ImVec2(8.0f * dpiScale, 8.0f * dpiScale));
                ImGui::TextWrapped("%s", info.changelog.c_str());
                ImGui::PopStyleVar();
                ImGui::EndChild();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // --- Buttons ---
            float buttonWidth  = 140.0f * dpiScale;
            float totalButtons = info.downloadUrl.empty() ? 1.0f : 2.0f;
            float spacing      = ImGui::GetStyle().ItemSpacing.x;
            float totalWidth =
                totalButtons * buttonWidth + (totalButtons - 1) * spacing;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalWidth) * 0.5f);

            if ( !info.downloadUrl.empty() ) {
                if ( ImGui::Button(TR("ui.help.download_and_install").data(),
                                   ImVec2(buttonWidth, 36.0f * dpiScale)) ) {
                    m_updateChecker->downloadAsync();
                }
                ImGui::SameLine();
            }

            if ( ImGui::Button(TR("ui.help.cancel").data(),
                               ImVec2(buttonWidth, 36.0f * dpiScale)) ) {
                ImGui::CloseCurrentPopup();
            }
        } else if ( info.status == MMM::Network::UpdateStatus::kDownloading ) {
            float textWidth =
                ImGui::CalcTextSize(TR("ui.help.downloading").data()).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
            ImGui::TextUnformatted(TR("ui.help.downloading").data());

            float barWidth = 360.0f * dpiScale;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - barWidth) * 0.5f);
            ImGui::ProgressBar(static_cast<float>(info.downloadProgress),
                               ImVec2(barWidth, 20.0f * dpiScale));

            char progressText[128];
            if ( info.downloadSize > 0 ) {
                snprintf(progressText,
                         sizeof(progressText),
                         "%lld / %lld MB",
                         (long long)(info.downloadedBytes / (1024 * 1024)),
                         (long long)(info.downloadSize / (1024 * 1024)));
            } else {
                snprintf(progressText,
                         sizeof(progressText),
                         "%.0f%%",
                         info.downloadProgress * 100.0);
            }
            float pWidth = ImGui::CalcTextSize(progressText).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - pWidth) * 0.5f);
            ImGui::TextDisabled("%s", progressText);
        } else if ( info.status == MMM::Network::UpdateStatus::kDownloaded ) {
            float textWidth =
                ImGui::CalcTextSize(TR("ui.help.download_complete").data()).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
            ImGui::TextUnformatted(TR("ui.help.download_complete").data());

            float btnWidth = 200.0f * dpiScale;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
            if ( ImGui::Button(TR("ui.help.restart_to_update").data(),
                               ImVec2(btnWidth, 40.0f * dpiScale)) ) {
                MMM::Network::UpdateChecker::applyUpdateAndRestart(
                    info.downloadedFilePath, info.updaterFilePath);
            }
        } else if ( info.status == MMM::Network::UpdateStatus::kError ) {
            ImVec4 errColor(1.0f, 0.4f, 0.4f, 1.0f);
            float  textWidth =
                ImGui::CalcTextSize(TR("ui.help.update_error").data()).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
            ImGui::TextColored(
                errColor, "%s", TR("ui.help.update_error").data());

            ImGui::PushStyleColor(
                ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", info.errorMessage.c_str());
            ImGui::PopStyleColor();

            float btnWidth = 120.0f * dpiScale;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
            if ( ImGui::Button(TR("ui.help.ok").data(),
                               ImVec2(btnWidth, 32.0f * dpiScale)) ) {
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::PopStyleVar(2);
        ImGui::EndPopup();
    }
}

void MainMenuView::renderUpdateSuccessPopup()
{
    if ( m_showUpdateSuccessPopup ) {
        ImGui::OpenPopup(TR("ui.help.update_success"));
        m_showUpdateSuccessPopup = false;
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if ( ImGui::BeginPopupModal(
             TR("ui.help.update_success"),
             nullptr,
             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize) ) {
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(32.0f * dpiScale, 24.0f * dpiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(8.0f * dpiScale, 16.0f * dpiScale));

        ImVec4 greenColor(0.3f, 1.0f, 0.3f, 1.0f);
        float  textWidth =
            ImGui::CalcTextSize(TR("ui.help.update_success_msg").data()).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
        ImGui::TextColored(
            greenColor, "%s", TR("ui.help.update_success_msg").data());

        // --- Info Table ---
        if ( ImGui::BeginTable("SuccessInfoTable",
                               2,
                               ImGuiTableFlags_SizingFixedFit |
                                   ImGuiTableFlags_NoSavedSettings) ) {
            ImGui::TableSetupColumn(
                "L", ImGuiTableColumnFlags_WidthFixed, 140.0f * dpiScale);
            ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(TR("ui.help.current_version").data());
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(greenColor, MMM_VERSION_STRING);

            ImGui::EndTable();
        }

        ImGui::Spacing();

        float btnWidth = 120.0f * dpiScale;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - btnWidth) * 0.5f);
        if ( ImGui::Button(TR("ui.help.ok").data(),
                           ImVec2(btnWidth, 32.0f * dpiScale)) ) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::PopStyleVar(2);
        ImGui::EndPopup();
    }
}

void MainMenuView::update(UIManager* sourceManager)
{
    // 启动时自动检查更新
    if ( !m_hasCheckedOnStartup ) {
        m_hasCheckedOnStartup = true;

        // 先检查是否刚完成更新
        if ( MMM::Network::UpdateChecker::checkStartupUpdateMarker() ) {
            m_showUpdateSuccessPopup = true;
        } else {
            m_showCheckingPopup = true;
            m_updateChecker->checkAsync();
        }
    }

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
                    std::filesystem::path p = Config::utf8ToPath(path);
                    std::string name        = Config::pathToUtf8(p.filename());
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
                 ICON_MMM_SAVE, TR("ui.file.save_as"), "Ctrl+Shift+S") ) {
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
        if ( MenuItemWithFontIcon(
                 ICON_MMM_PASTE, TR("ui.edit.paste"), "Ctrl+V") ) {
            dispatchCommand(Logic::CmdPaste{});
        }
        if ( MenuItemWithFontIcon(
                 ICON_MMM_MIRROR, TR("ui.edit.mirror"), "Ctrl+M") ) {
            dispatchCommand(Logic::CmdMirrorSelected{});
        }
        ImGui::Separator();
        if ( MenuItemWithFontIcon(
                 ICON_MMM_SELECT_ALL, TR("ui.edit.select_all"), "Ctrl+A") ) {
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
    renderUpdateSuccessPopup();

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
