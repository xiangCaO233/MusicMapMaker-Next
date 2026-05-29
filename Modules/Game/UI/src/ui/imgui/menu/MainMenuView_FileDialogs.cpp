#define IMGUI_DEFINE_MATH_OPERATORS
#include "canvas/TimeFormatUtils.h"
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/project/ProjectEvents.h"
#include "event/ui/UISettingsTabEvent.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Hold.h"
#include "mmm/note/Polyline.h"
#include "mmmversion.h"
#include "network/UpdateChecker.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/imgui/menu/MainMenuView.h"
#include <ImGuiFileDialog.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <nfd.h>

namespace MMM::UI
{

/// @brief 打开项目目录选择器并发布打开项目事件。
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

/// @brief 打开谱面打包保存路径选择器。
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

/// @brief 打开音频导入选择器并发布导入事件。
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

/// @brief 打开谱面导出保存路径选择器。
/// @param ext 期望导出的文件扩展名；为空时展示全部支持格式。
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

}  // namespace MMM::UI
