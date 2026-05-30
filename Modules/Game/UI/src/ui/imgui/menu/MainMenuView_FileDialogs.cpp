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
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <nfd.h>

namespace MMM::UI
{
namespace
{
/// @brief 将 ASCII 字符串转换为小写，用于扩展名判断。
std::string toLowerAscii(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

/// @brief 获取 UTF-8 路径的小写扩展名。
std::string getLowerExtension(const std::string& path)
{
    return toLowerAscii(
        Config::pathToUtf8(Config::utf8ToPath(path).extension()));
}

/// @brief 从文件选择器过滤器文本中解析扩展名。
std::string extensionFromFilterText(const std::string& filterText)
{
    const std::string lower = toLowerAscii(filterText);
    struct Candidate {
        /// @brief 目标扩展名。
        std::string extension;
        /// @brief 在过滤器文本中的位置。
        size_t position{ std::string::npos };
    };

    std::vector<Candidate> candidates = {
        { ".mmm", lower.find(".mmm") },
        { ".osu", lower.find(".osu") },
        { ".imd", lower.find(".imd") },
        { ".mc", lower.find(".mc") },
    };

    auto updateAlias = [&](const std::string& extension,
                           const std::string& alias) {
        size_t aliasPos = lower.find(alias);
        if ( aliasPos == std::string::npos ) return;
        for ( auto& candidate : candidates ) {
            if ( candidate.extension == extension &&
                 aliasPos < candidate.position ) {
                candidate.position = aliasPos;
            }
        }
    };
    updateAlias(".mmm", "musicmapmaker");
    updateAlias(".osu", "osu");
    updateAlias(".imd", "imd");
    updateAlias(".mc", "malody");

    const auto best = std::min_element(
        candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return a.position < b.position;
        });
    if ( best != candidates.end() && best->position != std::string::npos ) {
        return best->extension;
    }
    return {};
}

/// @brief 替换文件名中不适合作为普通文件名的路径分隔字符。
std::string sanitizeExportFileNamePart(std::string value)
{
    if ( value.empty() ) return "map";
    std::replace(value.begin(), value.end(), '/', '_');
    std::replace(value.begin(), value.end(), '\\', '_');
    return value;
}

/// @brief 判断谱面是否包含 IMD 无法保存的基础元数据。
bool hasUnsupportedImdBaseMetadata(const BaseMapMeta& meta)
{
    return !meta.title.empty() || !meta.title_unicode.empty() ||
           !meta.artist.empty() || !meta.artist_unicode.empty() ||
           !meta.author.empty() || !meta.main_audio_path.empty() ||
           !meta.main_cover_path.empty() || !meta.cover_path.empty() ||
           meta.video_starttime != 0 || meta.bgxoffset != 0 ||
           meta.bgyoffset != 0;
}

/// @brief 判断谱面是否包含 IMD 无法保存的谱面扩展元数据。
bool hasUnsupportedImdMapMetadata(const MapMetadata& metadata)
{
    for ( const auto& [source, properties] : metadata.map_properties ) {
        if ( properties.empty() ) continue;
        if ( source != MapMetadataType::RM ) return true;
        for ( const auto& [key, value] : properties ) {
            (void)value;
            if ( key != "mapLength" && key != "tabRows" ) return true;
        }
    }
    return false;
}

/// @brief 判断谱面物件是否包含 IMD 无法保存的额外物件元数据。
bool hasUnsupportedImdNoteMetadata(const BeatMap& beatMap)
{
    for ( const auto& noteRef : beatMap.m_allNotes ) {
        const auto& note = noteRef.get();
        for ( const auto& [source, properties] :
              note.m_metadata.note_properties ) {
            if ( properties.empty() ) continue;
            if ( source != NoteMetadataType::RM ) return true;
            for ( const auto& [key, value] : properties ) {
                (void)value;
                if ( key != "Parameter" ) return true;
            }
        }
    }
    return false;
}
}  // namespace

/// @brief 根据导出格式生成推荐文件名。
/// @param extension 目标扩展名。
/// @param currentFileName 当前文件名，用于保留非 IMD 格式的主文件名。
/// @return 推荐文件名。
std::string MainMenuView::makeExportFileNameForExtension(
    const std::string& extension, const std::string& currentFileName) const
{
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    const BeatMap* beatMap = (session && session->getContext().currentBeatmap)
                                 ? session->getContext().currentBeatmap.get()
                                 : nullptr;

    const std::string normalizedExt =
        extension.empty() ? ".mmm" : toLowerAscii(extension);

    if ( normalizedExt == ".imd" ) {
        std::string title    = "map";
        int32_t     keyCount = 0;
        std::string version  = "default";
        if ( beatMap ) {
            const auto& meta = beatMap->m_baseMapMetadata;
            title    = !meta.title_unicode.empty()
                           ? meta.title_unicode
                           : (!meta.title.empty() ? meta.title : meta.name);
            keyCount = meta.track_count;
            version  = meta.version.empty() ? "default" : meta.version;
        }
        return fmt::format("{}_{}k_{}.imd",
                           sanitizeExportFileNamePart(title),
                           keyCount,
                           sanitizeExportFileNamePart(version));
    }

    std::filesystem::path fileName = Config::utf8ToPath(currentFileName);
    if ( fileName.empty() ) {
        std::string baseName = "map";
        if ( beatMap && !beatMap->m_baseMapMetadata.name.empty() ) {
            baseName = beatMap->m_baseMapMetadata.name;
        }
        fileName = Config::utf8ToPath(sanitizeExportFileNamePart(baseName));
    }
    fileName.replace_extension(normalizedExt);
    return Config::pathToUtf8(fileName.filename());
}

/// @brief 按统一导出文件选择器当前格式规范化保存路径。
/// @param path 文件选择器返回的路径。
/// @return 应实际导出的目标路径。
std::string MainMenuView::applySaveAsSelectedFormatToPath(
    const std::string& path) const
{
    std::string currentFilter = ImGuiFileDialog::Instance()->GetCurrentFilter();
    std::string currentExtension = extensionFromFilterText(currentFilter);
    if ( currentExtension.empty() ) {
        return path;
    }

    std::filesystem::path outputPath = Config::utf8ToPath(path);
    std::string currentFileName = Config::pathToUtf8(outputPath.filename());
    std::string nextFileName =
        makeExportFileNameForExtension(currentExtension, currentFileName);
    outputPath.replace_filename(Config::utf8ToPath(nextFileName));
    return Config::pathToUtf8(outputPath);
}

/// @brief 直接分发谱面导出命令并显示保存提示。
/// @param path 目标导出路径。
void MainMenuView::dispatchSaveBeatmapAs(const std::string& path)
{
    dispatchCommand(Logic::CmdSaveBeatmapAs{ path });
}

/// @brief 收集当前谱面导出到指定格式时需要提醒用户的兼容性问题。
/// @param path 目标导出路径。
/// @return 需要展示的警告消息列表。
std::vector<std::string> MainMenuView::collectExportCompatibilityWarnings(
    const std::string& path) const
{
    std::vector<std::string> warnings;
    const std::string        ext = getLowerExtension(path);
    if ( ext != ".osu" && ext != ".imd" ) return warnings;

    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( !session || !session->getContext().currentBeatmap ) return warnings;

    const BeatMap& beatMap = *session->getContext().currentBeatmap;

    bool hasJumpOrHsTiming        = false;
    bool hasNegativeScrollTiming  = false;
    bool hasAnyNonBpmTimingForImd = false;
    bool hasFlick                 = !beatMap.m_noteData.flicks.empty();
    bool hasPolyline              = !beatMap.m_noteData.polylines.empty();
    bool hasUnsupportedBaseMeta   = false;
    bool hasUnsupportedMapMeta    = false;
    bool hasUnsupportedNoteMeta   = false;

    for ( const auto& timing : beatMap.m_timings ) {
        if ( timing.m_timingEffect == TimingEffect::JUMP ||
             timing.m_timingEffect == TimingEffect::HS ) {
            hasJumpOrHsTiming        = true;
            hasAnyNonBpmTimingForImd = true;
        } else if ( timing.m_timingEffect == TimingEffect::SCROLL ) {
            hasAnyNonBpmTimingForImd = true;
            if ( timing.m_timingEffectParameter < 0.0 ) {
                hasNegativeScrollTiming = true;
            }
        }
    }

    if ( ext == ".osu" ) {
        if ( hasJumpOrHsTiming ) {
            warnings.push_back(
                "osu! 文件不支持保存 Jump/HS timing；导出时这些 timing "
                "会被忽略。");
        }
        if ( hasNegativeScrollTiming ) {
            warnings.push_back(
                "osu! 文件不支持保存负数倍率的 Scroll timing；导出时该类 "
                "timing 会被忽略。");
        }
        if ( hasFlick ) {
            warnings.push_back(
                "Flick 物件会在 osu! 导出中自动转换为普通 Note。");
        }
        if ( hasPolyline ) {
            warnings.push_back(
                "Polyline 物件会在 osu! 导出中展开：其中 Flick "
                "子物件会被忽略，只导出其中所有 Hold。");
        }
    } else if ( ext == ".imd" ) {
        hasUnsupportedBaseMeta =
            hasUnsupportedImdBaseMetadata(beatMap.m_baseMapMetadata);
        hasUnsupportedMapMeta =
            hasUnsupportedImdMapMetadata(beatMap.m_metadata);
        hasUnsupportedNoteMeta = hasUnsupportedImdNoteMetadata(beatMap);

        if ( hasAnyNonBpmTimingForImd ) {
            warnings.push_back(
                "IMD 文件不支持保存 Jump/HS/Scroll timing；导出时只会保留 BPM "
                "timing。");
        }
        if ( hasUnsupportedBaseMeta || hasUnsupportedMapMeta ) {
            warnings.push_back(
                "IMD 文件不支持保存 "
                "title、artist、音频、封面等扩展元数据；仅保留 Version、key "
                "数、谱面时长、BPM timing 和物件数量/总数。");
        }
        if ( hasUnsupportedNoteMeta ) {
            warnings.push_back(
                "IMD "
                "文件不支持保存物件额外元数据；导出时只保留物件类型、时间、轨道"
                "和格式本身支持的参数。");
        }
    }

    return warnings;
}

/// @brief 请求导出当前谱面，必要时先展示格式兼容性警告。
/// @param path 目标导出路径。
void MainMenuView::requestSaveBeatmapAs(std::string path)
{
    auto warnings = collectExportCompatibilityWarnings(path);
    if ( warnings.empty() ) {
        dispatchSaveBeatmapAs(path);
        return;
    }

    const std::string ext   = getLowerExtension(path);
    m_pendingExportPath     = std::move(path);
    m_pendingExportWarnings = std::move(warnings);
    m_pendingExportFormatName =
        (ext == ".osu") ? "osu!" : ((ext == ".imd") ? "IMD" : "谱面");
    m_showExportCompatibilityWarning = true;
}

/// @brief 渲染导出兼容性警告弹窗。
/// @param dpiScale 当前窗口内容缩放。
void MainMenuView::renderExportCompatibilityWarningPopup(float dpiScale)
{
    constexpr const char* popupId = "导出兼容性警告###ExportWarningModal";
    if ( m_showExportCompatibilityWarning ) {
        ImGui::OpenPopup(popupId);
        m_showExportCompatibilityWarning = false;
    }

    if ( !ImGui::IsPopupOpen(popupId) ) return;

    ImGui::SetNextWindowSize(ImVec2(520.0f * dpiScale, 0.0f),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));

    if ( ImGui::BeginPopupModal(
             popupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize) ) {
        ImGui::Text("导出 %s 前需要确认以下兼容性变化：",
                    m_pendingExportFormatName.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        for ( const auto& warning : m_pendingExportWarnings ) {
            ImGui::BulletText("%s", warning.c_str());
        }

        ImGui::Spacing();
        ImGui::TextWrapped("目标文件：%s", m_pendingExportPath.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if ( ImGui::Button("继续导出", ImVec2(120.0f * dpiScale, 0.0f)) ) {
            dispatchSaveBeatmapAs(m_pendingExportPath);
            m_pendingExportPath.clear();
            m_pendingExportFormatName.clear();
            m_pendingExportWarnings.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button(TR("ui.common.cancel").data(),
                           ImVec2(120.0f * dpiScale, 0.0f)) ) {
            m_pendingExportPath.clear();
            m_pendingExportFormatName.clear();
            m_pendingExportWarnings.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

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
            defaultName = makeExportFileNameForExtension(".imd", defaultName);
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
            requestSaveBeatmapAs(outPath);
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
