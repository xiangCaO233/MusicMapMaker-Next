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
#include "mmm/project/PackageFileTypes.h"
#include "mmmversion.h"
#include "network/UpdateChecker.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/imgui/menu/MainMenuView.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <nfd.h>
#include <string_view>
#include <system_error>

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

/// @brief 将下一项控件放到当前内容区域的水平中心。
/// @param itemWidth 控件宽度。
/// @warning UI 绘制路径：只调整当前 ImGui 游标位置。
void centerNextItem(float itemWidth)
{
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    if ( availableWidth > itemWidth ) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             (availableWidth - itemWidth) * 0.5f);
    }
}

/// @brief 绘制水平居中的按钮。
/// @param label 按钮文本和 ImGui ID。
/// @param size 按钮尺寸。
/// @return 按钮被点击时返回 true。
/// @warning UI 绘制路径：只调整游标并调用 ImGui::Button。
bool drawCenteredButton(const char* label, ImVec2 size)
{
    centerNextItem(size.x);
    return ImGui::Button(label, size);
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

/// @brief 判断谱面是否包含 RM/IMD 无法保存的基础元数据。
bool hasUnsupportedImdBaseMetadata(const BaseMapMeta& meta)
{
    return !meta.title.empty() || !meta.title_unicode.empty() ||
           !meta.artist.empty() || !meta.artist_unicode.empty() ||
           !meta.author.empty() || !meta.main_audio_path.empty() ||
           !meta.main_cover_path.empty() || !meta.cover_path.empty() ||
           meta.video_starttime != 0 || meta.bgxoffset != 0 ||
           meta.bgyoffset != 0;
}

/// @brief 判断谱面是否包含 RM/IMD 无法保存的谱面扩展元数据。
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

/// @brief 判断谱面物件是否包含 RM/IMD 无法保存的额外物件元数据。
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

/// @brief 获取另存为对话框默认打开路径，优先使用当前项目根目录。
/// @param settings 编辑器设置，用于无项目时回退到通用文件选择器路径。
/// @return UTF-8 编码的默认目录路径。
std::string getSaveAsPickerDefaultPath(const Config::EditorSettings& settings)
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( project && !project->m_projectRoot.empty() ) {
        return Config::pathToUtf8(project->m_projectRoot);
    }

    return settings.lastFilePickerPath.empty() ? std::string(".")
                                               : settings.lastFilePickerPath;
}

/// @brief 获取打包格式显示名称。
/// @param type 打包格式。
/// @return 用户界面显示的格式名称。
std::string getPackageTypeDisplayName(PackageFileType type)
{
    const auto& types = getPackageSupportedFileTypes(type);
    switch ( type ) {
    case PackageFileType::Mcz:
        return "Malody Chart Package (" +
               std::string(types.m_packageExtension) + ")";
    case PackageFileType::Osz:
        return "osu! Beatmap Package (" +
               std::string(types.m_packageExtension) + ")";
    case PackageFileType::Mpk:
        return "MusicMapMaker Package (" +
               std::string(types.m_packageExtension) + ")";
    }
    return "MusicMapMaker Package (.mpk)";
}

/// @brief 取得打包格式扩展名。
/// @param type 打包格式。
/// @return 带前导点的扩展名。
std::string getPackageExtension(PackageFileType type)
{
    return std::string(getPackageSupportedFileTypes(type).m_packageExtension);
}

/// @brief 取得原生文件选择器使用的扩展名过滤器。
/// @param type 打包格式。
/// @return 不带前导点的扩展名。
const char* getNativePackageOutputFilterText(PackageFileType type)
{
    switch ( type ) {
    case PackageFileType::Mcz: return "mcz";
    case PackageFileType::Osz: return "osz";
    case PackageFileType::Mpk: return "mpk";
    }
    return "mpk";
}

/// @brief 取得统一文件选择器使用的扩展名过滤器。
/// @param type 打包格式。
/// @return 带前导点的扩展名。
const char* getUnifiedPackageOutputFilterText(PackageFileType type)
{
    switch ( type ) {
    case PackageFileType::Mcz: return ".mcz";
    case PackageFileType::Osz: return ".osz";
    case PackageFileType::Mpk: return ".mpk";
    }
    return ".mpk";
}

/// @brief 判断扩展名是否为打包产物扩展名。
/// @param extension 待检查扩展名。
/// @return 是否应从候选资源列表中排除。
bool isPackageArchiveExtension(const std::string& extension)
{
    return findPackageSupportedFileTypes(extension) != nullptr ||
           packageExtensionEquals(extension, ".zip");
}

/// @brief 根据扩展名推断资源分类显示文本。
/// @param types 当前打包格式规则。
/// @param extension 待检查扩展名。
/// @return 资源分类显示文本，空字符串表示不符合规则。
std::string getPackageCandidateTypeLabel(const PackageSupportedFileTypes& types,
                                         const std::string& extension)
{
    if ( isPackageBeatmapSourceExtensionSupported(types, extension) ) {
        if ( packageExtensionEquals(extension, ".mmm") &&
             !isPackageResourceExtensionSupported(
                 types, PackageResourceType::Beatmap, extension) ) {
            return "谱面源";
        }
        return "谱面";
    }
    if ( types.m_allowAllAudioFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Audio,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Audio, extension) ) {
        return "音频";
    }
    if ( types.m_allowAllVideoFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Video,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Video, extension) ) {
        return "视频";
    }
    if ( types.m_allowAllImageFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Image,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Image, extension) ) {
        return "图片";
    }
    return {};
}
}  // namespace

/// @brief 根据导出格式生成推荐文件名。
/// @param extension 目标扩展名。
/// @param currentFileName 当前文件名，用于保留非 RM/IMD 格式的主文件名。
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

/// @brief 按当前打包目标格式规范化输出包路径。
/// @param path 文件选择器返回的输出路径。
/// @return 补齐目标打包扩展名后的输出路径。
std::string MainMenuView::applyPackSelectedFormatToPath(
    const std::string& path) const
{
    if ( path.empty() ) return path;

    std::filesystem::path outputPath = Config::utf8ToPath(path);
    outputPath.replace_extension(
        getPackageExtension(m_selectedPackageFileType));
    return Config::pathToUtf8(outputPath);
}

/// @brief 直接分发谱面导出命令并显示保存提示。
/// @param path 目标导出路径。
void MainMenuView::dispatchSaveBeatmapAs(const std::string& path)
{
    dispatchCommand(Logic::CmdSaveBeatmapAs{ path });
}

/// @brief 请求打包当前已选择的项目文件。
/// @param path 输出包路径。
void MainMenuView::requestPackBeatmapTo(std::string path)
{
    path = applyPackSelectedFormatToPath(path);
    if ( path.empty() || m_pendingPackageRelativePaths.empty() ) {
        m_statusMessage      = "没有可打包的已选文件";
        m_statusMessageTimer = 3.0f;
        return;
    }

    dispatchCommand(Logic::CmdPackBeatmap{
        .exportPath                   = path,
        .selectedProjectRelativePaths = m_pendingPackageRelativePaths,
        .saveConvertedBeatmapsToProject =
            m_selectedPackageFileType == PackageFileType::Mcz &&
            m_saveConvertedPackageBeatmapsToProject,
    });
    m_pendingPackageRelativePaths.clear();
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
                "osu! 文件不支持负数 Scroll 倍率；导出时这些 Scroll timing "
                "会被跳过。");
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
                "RM 谱面格式不支持保存 Jump/HS/Scroll timing；导出时只会保留 "
                "BPM timing。");
        }
        if ( hasUnsupportedBaseMeta || hasUnsupportedMapMeta ) {
            warnings.push_back(
                "RM 谱面格式不支持保存 "
                "title、artist、音频、封面等扩展元数据；仅保留 Version、key "
                "数、谱面时长、BPM timing 和物件数量/总数。");
        }
        if ( hasUnsupportedNoteMeta ) {
            warnings.push_back(
                "RM "
                "谱面格式不支持保存物件额外元数据；导出时只保留物件类型、时间、"
                "轨道和格式本身支持的参数。");
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
        (ext == ".osu") ? "osu!" : ((ext == ".imd") ? "RM" : "谱面");
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

    {
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(520.0f * dpiScale, 0.0f)) ) {
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

            const ImVec2 actionButtonSize(120.0f * dpiScale, 0.0f);
            const float  actionButtonRowWidth =
                actionButtonSize.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            centerNextItem(actionButtonRowWidth);
            if ( ImGui::Button("继续导出", actionButtonSize) ) {
                dispatchSaveBeatmapAs(m_pendingExportPath);
                m_pendingExportPath.clear();
                m_pendingExportFormatName.clear();
                m_pendingExportWarnings.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.common.cancel").data(),
                               actionButtonSize) ) {
                m_pendingExportPath.clear();
                m_pendingExportFormatName.clear();
                m_pendingExportWarnings.clear();
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }
}

/// @brief 渲染原生另存为对话框前的导出格式选择弹窗。
/// @param dpiScale 当前窗口内容缩放。
void MainMenuView::renderExportFormatPickerPopup(float dpiScale)
{
    constexpr const char* popupId = "选择导出格式###ExportFormatPickerModal";
    if ( m_showExportFormatPicker ) {
        ImGui::OpenPopup(popupId);
        m_showExportFormatPicker = false;
    }

    if ( !ImGui::IsPopupOpen(popupId) ) return;

    std::string selectedExtension;
    {
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(360.0f * dpiScale, 0.0f)) ) {
            ImGui::TextUnformatted("选择另存为格式：");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImVec2 buttonSize(300.0f * dpiScale, 0.0f);
            if ( drawCenteredButton("MusicMapMaker Beatmap (.mmm)",
                                    buttonSize) ) {
                selectedExtension = ".mmm";
            }
            if ( drawCenteredButton("osu!mania Beatmap (.osu)", buttonSize) ) {
                selectedExtension = ".osu";
            }
            if ( drawCenteredButton("RM Beatmap (.imd)", buttonSize) ) {
                selectedExtension = ".imd";
            }
            if ( drawCenteredButton("Malody Chart (.mc)", buttonSize) ) {
                selectedExtension = ".mc";
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if ( drawCenteredButton(TR("ui.common.cancel").data(),
                                    ImVec2(120.0f * dpiScale, 0.0f)) ) {
                ImGui::CloseCurrentPopup();
            }

            if ( !selectedExtension.empty() ) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    if ( !selectedExtension.empty() ) {
        openExportFilePicker(selectedExtension);
    }
}

/// @brief 渲染打包目标格式选择弹窗。
/// @param dpiScale 当前窗口内容缩放。
void MainMenuView::renderPackageFormatPickerPopup(float dpiScale)
{
    constexpr const char* popupId = "选择打包格式###PackageFormatPickerModal";
    if ( m_showPackageFormatPicker ) {
        ImGui::OpenPopup(popupId);
        m_showPackageFormatPicker = false;
    }

    if ( !ImGui::IsPopupOpen(popupId) ) return;

    bool            hasSelection = false;
    PackageFileType selectedType = m_selectedPackageFileType;
    {
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(380.0f * dpiScale, 0.0f)) ) {
            ImGui::TextUnformatted("选择目标打包格式：");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImVec2 buttonSize(320.0f * dpiScale, 0.0f);
            if ( drawCenteredButton(
                     getPackageTypeDisplayName(PackageFileType::Mcz).c_str(),
                     buttonSize) ) {
                selectedType = PackageFileType::Mcz;
                hasSelection = true;
            }
            if ( drawCenteredButton(
                     getPackageTypeDisplayName(PackageFileType::Osz).c_str(),
                     buttonSize) ) {
                selectedType = PackageFileType::Osz;
                hasSelection = true;
            }
            if ( drawCenteredButton(
                     getPackageTypeDisplayName(PackageFileType::Mpk).c_str(),
                     buttonSize) ) {
                selectedType = PackageFileType::Mpk;
                hasSelection = true;
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if ( drawCenteredButton(TR("ui.common.cancel").data(),
                                    ImVec2(120.0f * dpiScale, 0.0f)) ) {
                ImGui::CloseCurrentPopup();
            }

            if ( hasSelection ) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    if ( hasSelection ) {
        m_selectedPackageFileType = selectedType;
        rebuildPackageCandidateFiles();
        m_showPackageFileSelectionWindow = true;
    }
}

/// @brief 渲染打包文件复选列表窗口。
/// @param dpiScale 当前窗口内容缩放。
void MainMenuView::renderPackageFileSelectionWindow(float dpiScale)
{
    constexpr const char* popupId = "选择打包文件###PackageFileSelectionModal";
    if ( m_showPackageFileSelectionWindow ) {
        ImGui::OpenPopup(popupId);
    }

    if ( !ImGui::IsPopupOpen(popupId) ) return;

    bool requestOutputPicker = false;
    bool closePopup          = false;
    {
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_NoCollapse,
                              ImVec2(760.0f * dpiScale, 540.0f * dpiScale),
                              false) ) {
            const auto selectedCount = static_cast<int>(
                std::count_if(m_packageCandidateFiles.begin(),
                              m_packageCandidateFiles.end(),
                              [](const PackageCandidateFile& file) {
                                  return file.selected;
                              }));
            ImGui::Text(
                "目标格式：%s",
                getPackageTypeDisplayName(m_selectedPackageFileType).c_str());
            ImGui::SameLine();
            ImGui::Text("已选择：%d / %d",
                        selectedCount,
                        static_cast<int>(m_packageCandidateFiles.size()));

            if ( ImGui::Button("全选", ImVec2(88.0f * dpiScale, 0.0f)) ) {
                for ( auto& file : m_packageCandidateFiles ) {
                    file.selected = true;
                }
            }
            ImGui::SameLine();
            if ( ImGui::Button("全不选", ImVec2(88.0f * dpiScale, 0.0f)) ) {
                for ( auto& file : m_packageCandidateFiles ) {
                    file.selected = false;
                }
            }
            if ( m_selectedPackageFileType == PackageFileType::Mcz ) {
                ImGui::SameLine();
                ImGui::Checkbox("保存转换出的 .mc 到项目中",
                                &m_saveConvertedPackageBeatmapsToProject);
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImGuiStyle& style = ImGui::GetStyle();
            const float       footerReserveHeight =
                ImGui::GetFrameHeightWithSpacing() +
                style.ItemSpacing.y * 4.0f + 2.0f * dpiScale;
            const float listHeight = std::max(
                48.0f * dpiScale,
                ImGui::GetContentRegionAvail().y - footerReserveHeight);

            if ( ImGui::BeginChild("PackageCandidateFilesChild",
                                   ImVec2(0.0f, listHeight),
                                   true) ) {
                if ( m_packageCandidateFiles.empty() ) {
                    ImGui::TextUnformatted(
                        "没有找到符合当前打包格式规则的文件。");
                } else {
                    constexpr ImGuiTableFlags tableFlags =
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_ScrollY;
                    if ( ImGui::BeginTable("PackageCandidateFilesTable",
                                           3,
                                           tableFlags,
                                           ImVec2(0.0f, 0.0f)) ) {
                        ImGui::TableSetupColumn(
                            "打包",
                            ImGuiTableColumnFlags_WidthFixed,
                            64.0f * dpiScale);
                        ImGui::TableSetupColumn(
                            "类型",
                            ImGuiTableColumnFlags_WidthFixed,
                            72.0f * dpiScale);
                        ImGui::TableSetupColumn(
                            "文件", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        for ( std::size_t index = 0;
                              index < m_packageCandidateFiles.size();
                              ++index ) {
                            auto& file = m_packageCandidateFiles[index];
                            ImGui::PushID(static_cast<int>(index));
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Checkbox("##PackageFileSelected",
                                            &file.selected);
                            ImGui::TableSetColumnIndex(1);
                            ImGui::TextUnformatted(file.typeLabel.c_str());
                            ImGui::TableSetColumnIndex(2);
                            ImGui::TextUnformatted(file.relativePath.c_str());
                            ImGui::PopID();
                        }

                        ImGui::EndTable();
                    }
                }
            }
            ImGui::EndChild();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const bool   canPack = selectedCount > 0;
            const ImVec2 footerButtonSize(120.0f * dpiScale, 0.0f);
            const float  footerButtonRowWidth =
                footerButtonSize.x * 2.0f + style.ItemSpacing.x;
            centerNextItem(footerButtonRowWidth);
            if ( !canPack ) ImGui::BeginDisabled();
            if ( ImGui::Button("打包到...", footerButtonSize) ) {
                m_pendingPackageRelativePaths =
                    collectSelectedPackageRelativePaths();
                requestOutputPicker = true;
                closePopup          = true;
            }
            if ( !canPack ) ImGui::EndDisabled();
            ImGui::SameLine();
            if ( ImGui::Button(TR("ui.common.cancel").data(),
                               footerButtonSize) ) {
                closePopup = true;
                m_pendingPackageRelativePaths.clear();
            }

            if ( closePopup ) {
                m_showPackageFileSelectionWindow = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    if ( requestOutputPicker ) {
        openPackageOutputFilePicker();
    }
}

/// @brief 按当前目标打包格式重建候选文件列表。
void MainMenuView::rebuildPackageCandidateFiles()
{
    m_packageCandidateFiles.clear();

    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || project->m_projectRoot.empty() ) return;

    const auto& types = getPackageSupportedFileTypes(m_selectedPackageFileType);
    const auto& projectRoot = project->m_projectRoot;

    std::error_code filesystemError;
    if ( !std::filesystem::exists(projectRoot, filesystemError) ||
         filesystemError ||
         !std::filesystem::is_directory(projectRoot, filesystemError) ||
         filesystemError ) {
        m_statusMessage      = "扫描项目文件失败";
        m_statusMessageTimer = 3.0f;
        return;
    }

    constexpr auto directoryOptions =
        std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator iterator(
        projectRoot, directoryOptions, filesystemError);
    const std::filesystem::recursive_directory_iterator endIterator;
    if ( filesystemError ) {
        m_statusMessage      = "扫描项目文件失败";
        m_statusMessageTimer = 3.0f;
        return;
    }

    while ( iterator != endIterator ) {
        const auto& entry = *iterator;
        if ( entry.is_regular_file(filesystemError) && !filesystemError ) {
            const auto path = entry.path();
            const auto extension =
                toLowerAscii(Config::pathToUtf8(path.extension()));
            const auto typeLabel =
                getPackageCandidateTypeLabel(types, extension);
            std::error_code relativeError;
            const auto      relativePath =
                std::filesystem::relative(path, projectRoot, relativeError);
            if ( !typeLabel.empty() && !isPackageArchiveExtension(extension) &&
                 !relativeError ) {
                m_packageCandidateFiles.push_back(PackageCandidateFile{
                    .relativePath = Config::pathToUtf8Generic(relativePath),
                    .typeLabel    = typeLabel,
                    .selected     = true,
                });
            }
        }
        filesystemError.clear();

        iterator.increment(filesystemError);
        if ( filesystemError ) {
            m_statusMessage      = "扫描项目文件失败";
            m_statusMessageTimer = 3.0f;
            break;
        }
    }

    std::sort(
        m_packageCandidateFiles.begin(),
        m_packageCandidateFiles.end(),
        [](const PackageCandidateFile& lhs, const PackageCandidateFile& rhs) {
            if ( lhs.typeLabel != rhs.typeLabel ) {
                return lhs.typeLabel < rhs.typeLabel;
            }
            return lhs.relativePath < rhs.relativePath;
        });
}

/// @brief 收集当前已勾选的项目相对文件路径。
/// @return 已勾选的项目相对文件路径列表。
std::vector<std::string>
MainMenuView::collectSelectedPackageRelativePaths() const
{
    std::vector<std::string> selectedPaths;
    selectedPaths.reserve(m_packageCandidateFiles.size());
    for ( const auto& file : m_packageCandidateFiles ) {
        if ( file.selected ) {
            selectedPaths.push_back(file.relativePath);
        }
    }
    return selectedPaths;
}

/// @brief 生成当前打包目标格式的默认输出文件名。
/// @return 默认输出文件名。
std::string MainMenuView::makePackageDefaultFileName() const
{
    std::string baseName = "map";
    auto*       project  = Logic::EditorEngine::instance().getCurrentProject();
    if ( project && !project->m_projectRoot.empty() ) {
        baseName = Config::pathToUtf8(project->m_projectRoot.filename());
    }
    if ( baseName.empty() ) baseName = "map";
    return sanitizeExportFileNamePart(baseName) +
           getPackageExtension(m_selectedPackageFileType);
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

/// @brief 打开谱面打包流程。
void MainMenuView::openPackFilePicker()
{
    m_packageCandidateFiles.clear();
    m_pendingPackageRelativePaths.clear();
    m_showPackageFileSelectionWindow = false;
    m_showPackageFormatPicker        = true;
}

/// @brief 打开打包输出路径选择器。
void MainMenuView::openPackageOutputFilePicker()
{
    auto& config = Config::AppConfig::instance().getEditorSettings();
    const std::string defaultFileName = makePackageDefaultFileName();
    const std::string defaultPath     = getSaveAsPickerDefaultPath(config);
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        nfdu8char_t* outPath = nullptr;
        const char*  packageFilter =
            getNativePackageOutputFilterText(m_selectedPackageFileType);
        nfdu8filteritem_t filters[1] = { { "Beatmap Package", packageFilter } };
        nfdresult_t       result     = NFD_SaveDialogU8(
            &outPath, filters, 1, defaultPath.c_str(), defaultFileName.c_str());

        if ( result == NFD_OKAY ) {
            requestPackBeatmapTo(outPath);
            NFD_FreePath(outPath);
        }
    } else {
        IGFD::FileDialogConfig fdConfig;
        fdConfig.path              = defaultPath;
        fdConfig.countSelectionMax = 1;
        fdConfig.fileName          = defaultFileName;
        fdConfig.flags =
            ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;
        const char* packageFilter =
            getUnifiedPackageOutputFilterText(m_selectedPackageFileType);
        ImGuiFileDialog::Instance()->OpenDialog(
            "PackFilePicker", TR("ui.file.pack"), packageFilter, fdConfig);
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
                                           "mp3,ogg,wav,flac,opus,aac,m4a" } };
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
            ".mp3,.ogg,.wav,.flac,.opus,.aac,.m4a",
            fdConfig);
    }
}

/// @brief 打开谱面导出保存路径选择器。
/// @param ext 期望导出的文件扩展名；为空时展示全部支持格式。
void MainMenuView::openExportFilePicker(const std::string& ext)
{
    auto& config = Config::AppConfig::instance().getEditorSettings();
    if ( config.filePickerStyle == Config::FilePickerStyle::Native &&
         ext.empty() ) {
        m_showExportFormatPicker = true;
        return;
    }

    const std::string defaultPath = getSaveAsPickerDefaultPath(config);

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
            filters[filterCount++] = { "RM Beatmap", "imd" };
        }
        if ( ext == ".mc" || ext == "" ) {
            filters[filterCount++] = { "Malody Chart", "mc" };
        }

        nfdresult_t result = NFD_SaveDialogU8(&outPath,
                                              filters,
                                              filterCount,
                                              defaultPath.c_str(),
                                              defaultName.c_str());

        if ( result == NFD_OKAY ) {
            requestSaveBeatmapAs(outPath);
            NFD_FreePath(outPath);
        }
    } else {
        IGFD::FileDialogConfig fdConfig;
        fdConfig.path              = defaultPath;
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
