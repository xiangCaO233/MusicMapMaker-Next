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
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Hold.h"
#include "mmm/note/Polyline.h"
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
    , m_openToolsMenuNextFrame(false)
    , m_openHelpMenuNextFrame(false)
    , m_closeFileMenuNextFrame(false)
    , m_closeEditMenuNextFrame(false)
    , m_closeToolsMenuNextFrame(false)
    , m_closeHelpMenuNextFrame(false)
    , m_showOverlapCheckWindow(false)
    , m_hasOverlapScan(false)
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

    // 如果 ImGui 当前处于文本输入状态，跳过全局快捷键处理以防冲突 (如 Ctrl+A
    // 全选)
    if ( io.WantTextInput ) return;

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
        if ( ImGui::IsKeyPressed(ImGuiKey_I, false) ) {
            openAudioImportPicker();
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_O) ) {
            openFolderPicker();
        }
        if ( ImGui::IsKeyPressed(ImGuiKey_S) ) {
            if ( io.KeyShift ) {
                openExportFilePicker("");
            } else {
                dispatchCommand(Logic::CmdSaveBeatmap{});
                m_saveTooltipTimer = 2.0f;
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
        if ( ImGui::IsKeyPressed(ImGuiKey_F, false) ) {
            dispatchCommand(Logic::CmdAlignSelectedToCommonBeats{});
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
        if ( ImGui::IsKeyPressed(ImGuiKey_T, false) ) {
            if ( ImGui::IsPopupOpen(TR("ui.tools")) ) {
                m_closeToolsMenuNextFrame = true;
            } else {
                m_openToolsMenuNextFrame = true;
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
        fdConfig.flags =
            ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;
        ImGuiFileDialog::Instance()->OpenDialog(
            "PackFilePicker", TR("ui.file.pack"), ".osz,.mcz,.zip", fdConfig);
    }
}

void MainMenuView::openAudioImportPicker()
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) return;

    auto& config = Config::AppConfig::instance().getEditorSettings();
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        nfdu8char_t*      outPath    = nullptr;
        nfdu8filteritem_t filters[1] = { { "Audio Files",
                                           "mp3,ogg,wav,flac" } };
        nfdresult_t result = NFD_OpenDialogU8(&outPath, filters, 1, nullptr);

        if ( result == NFD_OKAY ) {
            Event::EventBus::instance().publish(
                Event::AudioImportTriggerEvent{ outPath });
            NFD_FreePath(outPath);
        } else if ( result == NFD_ERROR ) {
            XERROR("NFD Error: {}", NFD_GetError());
        }
    } else {
        IGFD::FileDialogConfig fdConfig;
        fdConfig.path              = config.lastFilePickerPath;
        fdConfig.countSelectionMax = 1;
        fdConfig.fileName          = "";
        fdConfig.flags             = ImGuiFileDialogFlags_Modal |
                                     ImGuiFileDialogFlags_HideColumnType |
                                     ImGuiFileDialogFlags_ReadOnlyFileNameField;
        ImGuiFileDialog::Instance()->OpenDialog(
            "AudioImportPicker",
            TR("ui.audio_manager.import_audio").data(),
            ".mp3,.ogg,.wav,.flac",
            fdConfig);
    }
}

void MainMenuView::openExportFilePicker(const std::string& ext)
{
    auto& config = Config::AppConfig::instance().getEditorSettings();

    std::string defaultName = "map" + (ext.empty() ? ".mmm" : ext);
    auto&       engine      = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
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
        fdConfig.flags =
            ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;

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
    m_isSilentCheck     = false;
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
    {
        static bool wasOpen = false;
        bool        isOpen  = ImGui::IsPopupOpen(TR("ui.help.about_title"));
        if ( isOpen && !wasOpen ) {
            ImGui::SetNextWindowPos(
                center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }
        wasOpen = isOpen;
    }

    if ( ImGui::BeginPopupModal(
             TR("ui.help.about_title"), nullptr, ImGuiWindowFlags_None) ) {
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
    {
        static bool wasOpen = false;
        bool        isOpen  = ImGui::IsPopupOpen(TR("ui.help.check_update"));
        if ( isOpen && !wasOpen ) {
            ImGui::SetNextWindowPos(
                center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }
        wasOpen = isOpen;
    }

    bool open = true;
    if ( ImGui::BeginPopupModal(
             TR("ui.help.check_update"), &open, ImGuiWindowFlags_None) ) {
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
    {
        static bool wasOpen = false;
        bool        isOpen  = ImGui::IsPopupOpen(TR("ui.help.update_found"));
        if ( isOpen && !wasOpen ) {
            ImGui::SetNextWindowPos(
                center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }
        wasOpen = isOpen;
    }

    bool isWorking = (info.status == MMM::Network::UpdateStatus::kDownloading ||
                      info.status == MMM::Network::UpdateStatus::kDownloaded);
    bool open      = true;
    if ( ImGui::BeginPopupModal(TR("ui.help.update_found"),
                                isWorking ? nullptr : &open,
                                ImGuiWindowFlags_None) ) {
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
    {
        static bool wasOpen = false;
        bool        isOpen  = ImGui::IsPopupOpen(TR("ui.help.update_success"));
        if ( isOpen && !wasOpen ) {
            ImGui::SetNextWindowPos(
                center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        }
        wasOpen = isOpen;
    }

    if ( ImGui::BeginPopupModal(
             TR("ui.help.update_success"), nullptr, ImGuiWindowFlags_None) ) {
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

void MainMenuView::renderSaveTooltip()
{
    if ( m_saveTooltipTimer <= 0.0f ) return;

    m_saveTooltipTimer -= ImGui::GetIO().DeltaTime;

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2         mousePos = ImGui::GetMousePos();

    // 始终跟随鼠标，并根据屏幕位置自动调整对齐方式（边缘翻转）
    ImVec2 pivot = ImVec2(0.0f, 0.0f);
    if ( mousePos.x > viewport->WorkPos.x + viewport->WorkSize.x * 0.7f )
        pivot.x = 1.0f;
    if ( mousePos.y > viewport->WorkPos.y + viewport->WorkSize.y * 0.7f )
        pivot.y = 1.0f;

    float offsetX = (pivot.x == 0.0f) ? 20.0f : -20.0f;
    float offsetY = (pivot.y == 0.0f) ? 20.0f : -20.0f;

    ImGui::SetNextWindowPos(ImVec2(mousePos.x + offsetX, mousePos.y + offsetY),
                            ImGuiCond_Always,
                            pivot);

    ImGui::SetNextWindowBgAlpha(0.8f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 10));

    if ( ImGui::Begin("##SaveTooltip", nullptr, flags) ) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                           "%s  %s",
                           ICON_MMM_SAVE,
                           TR("ui.status.beatmap.saved").data());
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void MainMenuView::update(UIManager* sourceManager)
{
    if ( m_statusMessageTimer > 0.0f )
        m_statusMessageTimer -= ImGui::GetIO().DeltaTime;

    // 启动时自动检查更新
    if ( !m_hasCheckedOnStartup ) {
        m_hasCheckedOnStartup = true;

        // 先检查是否刚完成更新
        if ( MMM::Network::UpdateChecker::checkStartupUpdateMarker() ) {
            m_showUpdateSuccessPopup = true;
        } else {
            m_isSilentCheck = true;  // 静默检查
            m_updateChecker->checkAsync();
        }
    }

    // 如果是静默检查，监测状态
    if ( m_isSilentCheck ) {
        auto info = m_updateChecker->getInfo();
        if ( info.status == MMM::Network::UpdateStatus::kUpdateFound ) {
            m_showUpdatePopup = true;
            m_isSilentCheck   = false;
        } else if ( info.status == MMM::Network::UpdateStatus::kUpToDate ) {
            m_statusMessage      = TR("ui.help.up_to_date").data();
            m_statusMessageTimer = 5.0f;
            m_isSilentCheck      = false;
        } else if ( info.status == MMM::Network::UpdateStatus::kError ) {
            m_isSilentCheck = false;
        }
    }

    renderSaveTooltip();
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

        if ( MenuItemWithFontIcon(ICON_MMM_MUSIC,
                                  TR("ui.audio_manager.import_audio"),
                                  "Ctrl+I",
                                  hasProject) ) {
            openAudioImportPicker();
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
            m_saveTooltipTimer = 2.0f;
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

    // ========== Tools Menu ==========
    if ( m_openToolsMenuNextFrame ) {
        ImGui::OpenPopup(TR("ui.tools"));
        m_openToolsMenuNextFrame = false;
    }
    if ( ImGui::BeginMenu(TR("ui.tools")) ) {
        if ( m_closeToolsMenuNextFrame ) {
            ImGui::CloseCurrentPopup();
            m_closeToolsMenuNextFrame = false;
        }

        if ( MenuItemWithFontIcon(ICON_MMM_SELECT_ALL,
                                  TR("ui.tools.overlap_check")) ) {
            m_showOverlapCheckWindow = !m_showOverlapCheckWindow;
        }

        if ( MenuItemWithFontIcon(
                 ICON_MMM_BARS, TR("ui.tools.format"), "Ctrl+F") ) {
            dispatchCommand(Logic::CmdAlignSelectedToCommonBeats{});
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
    renderOverlapCheckWindow();

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

void MainMenuView::performOverlapScan()
{
    m_overlapResults.clear();
    m_hasOverlapScan = true;

    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( !session ) return;

    struct CheckItem {
        ::MMM::NoteType type;
        double          start_time;
        double          end_time;
        int             track;
        entt::entity    entity;
        entt::entity    parent_polyline;
        std::string     desc;
    };

    std::vector<CheckItem> items;
    const auto&            registry = session->getContext().noteRegistry;
    auto                   view = registry.view<const Logic::NoteComponent>();

    for ( auto entity : view ) {
        const auto& nc = view.get<const Logic::NoteComponent>(entity);

        // Skip Polyline container entities because its individual subnotes are
        // separate entities checked below
        if ( nc.m_type == ::MMM::NoteType::POLYLINE ) continue;

        double startTime = nc.m_timestamp;
        double endTime   = startTime;
        if ( nc.m_type == ::MMM::NoteType::HOLD ) {
            endTime = startTime + nc.m_duration;
        }

        std::string desc = "Note";
        if ( nc.m_type == ::MMM::NoteType::HOLD ) {
            desc = nc.m_isSubNote ? "Polyline Hold" : "Hold";
        } else if ( nc.m_type == ::MMM::NoteType::FLICK ) {
            desc = nc.m_isSubNote ? "Polyline Flick" : "Flick";
        }

        items.push_back({ nc.m_type,
                          startTime,
                          endTime,
                          nc.m_trackIndex,
                          entity,
                          nc.m_parentPolyline,
                          desc });
    }

    // Sort items by track, then by start_time
    std::sort(
        items.begin(), items.end(), [](const CheckItem& x, const CheckItem& y) {
            if ( x.track != y.track ) return x.track < y.track;
            return x.start_time < y.start_time;
        });

    // DSU structure for grouping contiguous overlapping notes
    struct DSU {
        std::vector<int> parent;
        DSU(size_t n)
        {
            parent.resize(n);
            for ( size_t i = 0; i < n; ++i ) parent[i] = i;
        }
        int find(int i)
        {
            if ( parent[i] == i ) return i;
            return parent[i] = find(parent[i]);
        }
        void unite(int i, int j)
        {
            int root_i = find(i);
            int root_j = find(j);
            if ( root_i != root_j ) {
                parent[root_i] = root_j;
            }
        }
    };

    DSU               dsu(items.size());
    std::vector<bool> isDefiniteOverlap(items.size(), false);
    std::vector<bool> hasAnyOverlap(items.size(), false);

    // Sweep-line with sliding window of max 10ms (0.010s) suspicion window
    for ( size_t i = 0; i < items.size(); ++i ) {
        const auto& a              = items[i];
        double      max_check_time = std::max(a.start_time, a.end_time) + 0.010;

        for ( size_t j = i + 1; j < items.size(); ++j ) {
            const auto& b = items[j];

            // If we hit a different track, stop search since it's sorted by
            // track
            if ( a.track != b.track ) break;

            // If start_time of b is beyond max_check_time, stop search since
            // it's sorted by start_time
            if ( b.start_time > max_check_time ) break;

            // Must not belong to the same Polyline
            if ( a.parent_polyline != entt::null &&
                 a.parent_polyline == b.parent_polyline )
                continue;

            double t1_start = a.start_time;
            double t1_end   = a.end_time;
            double t2_start = b.start_time;
            double t2_end   = b.end_time;

            // Sorted order guarantees t1_start <= t2_start
            double diff_start = t2_start - t1_start;

            bool is_definite  = false;
            bool is_suspected = false;

            // Strict time check in seconds (1ms = 0.001s, 10ms = 0.010s)
            if ( diff_start < 0.001 ) {
                is_definite = true;
            } else if ( t2_start > t1_start + 0.001 &&
                        t2_start < t1_end - 0.001 ) {
                is_definite = true;
            } else if ( diff_start >= 0.001 && diff_start <= 0.010 ) {
                is_suspected = true;
            } else if ( std::abs(t1_end - t2_start) >= 0.001 &&
                        std::abs(t1_end - t2_start) <= 0.010 ) {
                is_suspected = true;
            }

            if ( is_definite || is_suspected ) {
                dsu.unite(i, j);
                hasAnyOverlap[i] = true;
                hasAnyOverlap[j] = true;
                if ( is_definite ) {
                    isDefiniteOverlap[i] = true;
                    isDefiniteOverlap[j] = true;
                }
            }
        }
    }

    // Gather items into groups by DSU root
    std::unordered_map<int, std::vector<size_t>> groups;
    for ( size_t i = 0; i < items.size(); ++i ) {
        if ( hasAnyOverlap[i] ) {
            groups[dsu.find(i)].push_back(i);
        }
    }

    // Build the finalized grouped overlap results
    for ( const auto& pair : groups ) {
        const auto& indices = pair.second;
        if ( indices.size() < 2 ) continue;

        // Find the earliest start time, track, and whether the group is
        // definite
        double min_time    = items[indices[0]].start_time;
        int    track       = items[indices[0]].track;
        bool   is_definite = false;

        for ( size_t idx : indices ) {
            min_time = std::min(min_time, items[idx].start_time);
            if ( isDefiniteOverlap[idx] ) {
                is_definite = true;
            }
        }

        std::string desc1;
        std::string desc2;
        if ( indices.size() == 2 ) {
            desc1 = items[indices[0]].desc;
            desc2 = items[indices[1]].desc;
        } else {
            desc1 = TR_FMT("ui.tools.multiple_objects", indices.size());
            desc2 = TR("ui.tools.each_other").data();
        }

        m_overlapResults.push_back({ is_definite,
                                     min_time,
                                     static_cast<uint32_t>(track),
                                     desc1,
                                     desc2 });
    }
}

void MainMenuView::renderOverlapCheckWindow()
{
    if ( !m_showOverlapCheckWindow ) return;

    auto& editorSettings = Config::AppConfig::instance().getEditorSettings();
    float dpiScale = Config::AppConfig::instance().getWindowContentScale();
    float windowRound =
        std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
    float frameRound =
        std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
    ImVec2 itemSpacing = {
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
        std::floor(editorSettings.aesthetics.itemSpacing * dpiScale)
    };

    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
               std::floor(editorSettings.aesthetics.windowPadding * dpiScale)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

    ImGui::SetNextWindowSize(ImVec2(550.0f * dpiScale, 450.0f * dpiScale),
                             ImGuiCond_FirstUseEver);

    auto&   skinMgr   = Config::SkinManager::instance();
    ImFont* titleFont = skinMgr.getFont("title");
    if ( titleFont ) ImGui::PushFont(titleFont);

    bool opened = ImGui::Begin(TR("ui.tools.overlap_check_title").data(),
                               &m_showOverlapCheckWindow,
                               ImGuiWindowFlags_None);

    if ( titleFont ) ImGui::PopFont();

    if ( opened ) {
        auto& engine = Logic::EditorEngine::instance();
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        auto session = engine.getActiveSession();
        if ( !session ) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "%s",
                               TR("ui.tools.no_active_session").data());
        } else {
            auto beatmap = session->getContext().currentBeatmap;
            if ( !beatmap ) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "%s",
                                   TR("ui.tools.no_active_beatmap").data());
            } else {
                if ( !m_hasOverlapScan ) {
                    if ( ImGui::Button(TR("ui.tools.scan_now").data(),
                                       ImVec2(-1.0f, 40.0f * dpiScale)) ) {
                        performOverlapScan();
                    }
                } else {
                    if ( ImGui::Button(
                             TR("ui.tools.rescan").data(),
                             ImVec2(120.0f * dpiScale, 30.0f * dpiScale)) ) {
                        performOverlapScan();
                    }

                    ImGui::SameLine();
                    int definiteCount  = 0;
                    int suspectedCount = 0;
                    for ( const auto& r : m_overlapResults ) {
                        if ( r.is_definite )
                            definiteCount++;
                        else
                            suspectedCount++;
                    }

                    std::string summaryStr = TR_FMT(
                        "ui.tools.scan_summary", definiteCount, suspectedCount);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(summaryStr.c_str());

                    ImGui::Separator();
                    ImGui::Spacing();

                    if ( m_overlapResults.empty() ) {
                        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                                           "%s",
                                           TR("ui.tools.no_overlaps").data());
                    } else {
                        ImGuiTableFlags tableFlags =
                            ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersOuter |
                            ImGuiTableFlags_Resizable;
                        if ( ImGui::BeginTable("OverlapResultsTable",
                                               5,
                                               tableFlags,
                                               ImVec2(0.0f, -1.0f)) ) {
                            ImGui::TableSetupColumn(
                                TR("ui.tools.overlap_type").data(),
                                ImGuiTableColumnFlags_WidthFixed,
                                100.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                TR("ui.canvas.note_time").data(),
                                ImGuiTableColumnFlags_WidthFixed,
                                90.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                TR("ui.canvas.track").data(),
                                ImGuiTableColumnFlags_WidthFixed,
                                60.0f * dpiScale);
                            ImGui::TableSetupColumn(
                                TR("ui.tools.overlap_detail_header").data(),
                                ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn(
                                TR("ui.tools.overlap_jump_header").data(),
                                ImGuiTableColumnFlags_WidthFixed,
                                50.0f * dpiScale);

                            ImGui::TableHeadersRow();

                            ImGuiListClipper clipper;
                            clipper.Begin(
                                static_cast<int>(m_overlapResults.size()));
                            while ( clipper.Step() ) {
                                for ( int i = clipper.DisplayStart;
                                      i < clipper.DisplayEnd;
                                      ++i ) {
                                    const auto& r = m_overlapResults[i];
                                    ImGui::TableNextRow();

                                    // 1. Type
                                    ImGui::TableNextColumn();
                                    if ( r.is_definite ) {
                                        ImGui::TextColored(
                                            ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                                            "%s",
                                            TR("ui.tools.definite").data());
                                    } else {
                                        ImGui::TextColored(
                                            ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                            "%s",
                                            TR("ui.tools.suspected").data());
                                    }

                                    // 2. Time
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%.3f s", r.timestamp);

                                    // 3. Track
                                    ImGui::TableNextColumn();
                                    ImGui::Text("%d", r.track + 1);

                                    // 4. Detail
                                    ImGui::TableNextColumn();
                                    std::string detailStr =
                                        TR_FMT("ui.tools.overlap_detail",
                                               r.note1_desc,
                                               r.note2_desc);
                                    ImGui::TextUnformatted(detailStr.c_str());

                                    // 5. Jump Action
                                    ImGui::TableNextColumn();
                                    ImGui::PushStyleColor(ImGuiCol_Button,
                                                          ImVec4(0, 0, 0, 0));
                                    ImGui::PushStyleColor(
                                        ImGuiCol_ButtonHovered,
                                        ImVec4(0.4f, 0.7f, 1.0f, 0.3f));
                                    if ( ImGui::Button(
                                             fmt::format(
                                                 "{}##{}", ICON_MMM_SEARCH, i)
                                                 .c_str(),
                                             ImVec2(-1, 0)) ) {
                                        float visualOffset =
                                            Config::AppConfig::instance()
                                                .getVisualConfig()
                                                .getEffectiveVisualOffset();
                                        dispatchCommand(Logic::CmdSeek{
                                            r.timestamp - visualOffset });
                                    }
                                    ImGui::PopStyleColor(2);
                                    if ( ImGui::IsItemHovered() ) {
                                        ImGui::SetTooltip(
                                            "%s",
                                            TR_FMT("canvas.preview.jump_to",
                                                   r.timestamp)
                                                .c_str());
                                    }
                                }
                            }
                            ImGui::EndTable();
                        }
                    }
                }
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(6);
}

}  // namespace MMM::UI
