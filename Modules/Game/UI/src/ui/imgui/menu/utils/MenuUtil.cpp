#include "ui/imgui/menu/utils/MenuUtil.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/utils/UIWidgetUtils.h"

#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <fmt/format.h>
#include <mutex>
#include <nfd.h>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 去除 ASCII 空白，用于解析 Malody mode 元数据。
/// @param text 原始字符串视图。
/// @return 去除首尾空白后的字符串视图。
std::string_view trimAsciiWhitespace(std::string_view text)
{
    while ( !text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                              text.front() == '\n' || text.front() == '\r') ) {
        text.remove_prefix(1);
    }
    while ( !text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                              text.back() == '\n' || text.back() == '\r') ) {
        text.remove_suffix(1);
    }
    return text;
}

/// @brief 无异常解析整数字符串。
/// @param text 待解析文本。
/// @return 成功时返回整数，否则返回空。
std::optional<int> parseAsciiInteger(std::string_view text)
{
    text = trimAsciiWhitespace(text);
    if ( text.empty() ) return std::nullopt;

    int  value = 0;
    auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if ( result.ec != std::errc{} || result.ptr != text.data() + text.size() ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 获取谱面当前 Malody mode 元数据，缺省时按导出器默认 slide(7) 处理。
/// @param beatMap 当前谱面。
/// @return mode 元数据有效时返回 mode；无法解析时返回空。
std::optional<int> resolveMalodyModeForCompatibilityWarning(
    const BeatMap& beatMap)
{
    int mode = 7;
    if ( auto it =
             beatMap.m_metadata.map_properties.find(MapMetadataType::MALODY);
         it != beatMap.m_metadata.map_properties.end() ) {
        if ( it->second.contains("mode") ) {
            auto parsedMode = parseAsciiInteger(it->second.at("mode"));
            if ( !parsedMode ) return std::nullopt;
            mode = *parsedMode;
        }
    }
    return mode;
}

/// @brief 判断谱面是否包含需要上架皮肤 mode_ext 的 Malody 元素。
/// @param beatMap 当前谱面。
/// @return 含 Flick 或折线时返回 true。
bool hasMalodyStoreModeExtEligibleElements(const BeatMap& beatMap)
{
    return !beatMap.m_noteData.flicks.empty() ||
           !beatMap.m_noteData.polylines.empty();
}

/// @brief 从文件选择器过滤器文本中解析扩展名。
/// @param filterText 统一文件选择器当前过滤器文本。
/// @return 匹配到的扩展名；无法识别时返回空。
std::string extensionFromFilterText(const std::string& filterText)
{
    const std::string lower = MenuUtil::toLowerAscii(filterText);
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
/// @param value 原始文件名片段。
/// @return 可用于推荐文件名的文本。
std::string sanitizeExportFileNamePart(std::string value)
{
    if ( value.empty() ) return "map";
    std::replace(value.begin(), value.end(), '/', '_');
    std::replace(value.begin(), value.end(), '\\', '_');
    return value;
}

/// @brief 判断谱面是否包含 RM/IMD 无法保存的基础元数据。
/// @param meta 基础谱面元数据。
/// @return 存在不支持字段时返回 true。
bool hasUnsupportedImdBaseMetadata(const BaseMapMeta& meta)
{
    return !meta.title.empty() || !meta.title_unicode.empty() ||
           !meta.artist.empty() || !meta.artist_unicode.empty() ||
           !meta.author.empty() || !meta.song_file_hint.empty() ||
           !meta.main_audio_path.empty() || !meta.main_cover_path.empty() ||
           !meta.cover_path.empty() || meta.video_starttime != 0 ||
           meta.bgxoffset != 0 || meta.bgyoffset != 0;
}

/// @brief 判断谱面是否包含 RM/IMD 无法保存的谱面扩展元数据。
/// @param metadata 谱面扩展元数据。
/// @return 存在不支持字段时返回 true。
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
/// @param beatMap 当前谱面。
/// @return 存在不支持字段时返回 true。
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

/// @brief 发布逻辑命令事件。
/// @param cmd 需要分发给逻辑层的命令。
void MenuUtil::dispatchCommand(const Logic::LogicCommand& cmd)
{
    Event::EventBus::instance().publish(Event::LogicCommandEvent(cmd));
}

/// @brief 打开项目目录选择器并发布打开项目事件。
/// @warning 用户触发的低频路径：原生选择器可能阻塞。
void MenuUtil::openProjectFolderPicker()
{
    auto& config = Config::AppConfig::instance().getEditorSettings();
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t* outPath = nullptr;
        nfdresult_t  result  = NFD_PickFolder(&outPath, nullptr);

        if ( result == NFD_OKAY ) {
            Event::OpenProjectEvent ev;
            ev.m_projectPath = Config::utf8ToPath(outPath);
            Event::EventBus::instance().publish(ev);
            NFD_FreePath(outPath);
        }
        return;
    }

    IGFD::FileDialogConfig fdConfig;
    fdConfig.path              = config.lastFilePickerPath;
    fdConfig.countSelectionMax = 1;
    fdConfig.flags             = ImGuiFileDialogFlags_Modal;
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened("ProjectFolderPicker");
    ImGuiFileDialog::Instance()->OpenDialog(
        "ProjectFolderPicker",
        TR("ui.file_manager.open_directory"),
        nullptr,
        fdConfig);
    if ( !wasOpen &&
         ImGuiFileDialog::Instance()->IsOpened("ProjectFolderPicker") ) {
        ::MMM::UI::PlayPopupOpenFeedback();
    }
}

/// @brief 打开音频导入选择器并发布导入事件。
/// @warning 用户触发的低频路径：原生选择器可能阻塞。
void MenuUtil::openAudioImportPicker()
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) return;

    auto& config = Config::AppConfig::instance().getEditorSettings();
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        ::MMM::UI::PlayPopupOpenFeedback();
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
        return;
    }

    IGFD::FileDialogConfig fdConfig;
    fdConfig.path              = config.lastFilePickerPath;
    fdConfig.countSelectionMax = 1;
    fdConfig.fileName          = "";
    fdConfig.flags             = ImGuiFileDialogFlags_Modal |
                                 ImGuiFileDialogFlags_HideColumnType |
                                 ImGuiFileDialogFlags_ReadOnlyFileNameField;
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened("AudioImportPicker");
    ImGuiFileDialog::Instance()->OpenDialog(
        "AudioImportPicker",
        TR("ui.audio_manager.import_audio").data(),
        ".mp3,.ogg,.wav,.flac,.opus,.aac,.m4a",
        fdConfig);
    if ( !wasOpen &&
         ImGuiFileDialog::Instance()->IsOpened("AudioImportPicker") ) {
        ::MMM::UI::PlayPopupOpenFeedback();
    }
}

/// @brief 当前是否存在活跃谱面。
/// @param requireProject 是否同时要求当前项目存在。
/// @return 存在活跃谱面时返回 true。
/// @warning UI 热路径低频分支：仅在菜单展开或 action 判定时读取当前会话状态。
bool MenuUtil::hasActiveBeatmap(bool requireProject)
{
    auto& engine = Logic::EditorEngine::instance();
    if ( requireProject && !engine.getCurrentProject() ) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    return session && session->getContext().currentBeatmap;
}

/// @brief 当前是否允许触发画布编辑类快捷键。
/// @return 允许触发时返回 true。
/// @warning UI 热路径：每帧快捷键判断调用；只读取 ImGui 输入阻断状态。
bool MenuUtil::canTriggerCanvasEditingShortcut()
{
    return !ShortcutUtils::shouldBlockCanvasEditingShortcuts();
}

/// @brief 将 ASCII 字符串转换为小写。
/// @param value 原始字符串。
/// @return 转换后的字符串。
std::string MenuUtil::toLowerAscii(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

/// @brief 获取 UTF-8 路径的小写扩展名。
/// @param path UTF-8 路径字符串。
/// @return 小写扩展名。
std::string MenuUtil::lowerExtension(const std::string& path)
{
    return toLowerAscii(
        Config::pathToUtf8(Config::utf8ToPath(path).extension()));
}

/// @brief 根据导出格式生成推荐文件名。
/// @param extension 目标扩展名。
/// @param currentFileName 当前文件名，用于保留非 RM/IMD 格式的主文件名。
/// @return 推荐文件名。
std::string MenuUtil::makeExportFileNameForExtension(
    const std::string& extension, const std::string& currentFileName)
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
std::string MenuUtil::applySaveAsSelectedFormatToPath(const std::string& path)
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

/// @brief 获取另存为对话框默认打开路径。
/// @return UTF-8 编码的默认目录路径。
std::string MenuUtil::getSaveAsPickerDefaultPath()
{
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( project && !project->m_projectRoot.empty() ) {
        return Config::pathToUtf8(project->m_projectRoot);
    }

    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    return settings.lastFilePickerPath.empty() ? std::string(".")
                                               : settings.lastFilePickerPath;
}

/// @brief 收集当前谱面导出到指定格式时需要提醒用户的兼容性问题。
/// @param path 目标导出路径。
/// @return 需要展示的警告消息列表。
std::vector<std::string> MenuUtil::collectExportCompatibilityWarnings(
    const std::string& path)
{
    std::vector<std::string> warnings;
    const std::string        ext = lowerExtension(path);
    if ( ext != ".osu" && ext != ".imd" && ext != ".mc" ) return warnings;

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
    } else if ( ext == ".mc" ) {
        const auto mode = resolveMalodyModeForCompatibilityWarning(beatMap);
        if ( mode && *mode == 0 && (hasFlick || hasPolyline) ) {
            warnings.push_back(
                "Malody key(0) 模式无法存储 Flick/折线；继续保存会将 "
                "Flick 作为单 Note 写出，忽略 Polyline 中所有 subFlick，"
                "并将所有 subHold 作为普通 Hold "
                "写出，转换结果会覆盖目标谱面。");
        }
    }

    return warnings;
}

/// @brief 判断当前 MC 导出目标是否需要显示上架 mode_ext 选项。
/// @param path 目标导出路径。
/// @return 导出 MC 且当前谱面含 Flick/折线时返回 true。
bool MenuUtil::shouldOfferMalodyStoreModeExtForCurrentExport(
    const std::string& path)
{
    if ( lowerExtension(path) != ".mc" ) return false;

    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( !session || !session->getContext().currentBeatmap ) return false;

    return hasMalodyStoreModeExtEligibleElements(
        *session->getContext().currentBeatmap);
}

/// @brief 将项目谱面路径规范化为候选比较键。
/// @param projectRoot 当前项目根目录。
/// @param path 谱面路径，可为项目相对路径或绝对路径。
/// @return 规范化后的 UTF-8 路径键。
std::string MenuUtil::makeProjectBeatmapPathKey(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    if ( path.empty() ) return {};

    std::filesystem::path fullPath =
        path.is_absolute() ? path : (projectRoot / path);
    std::error_code filesystemError;
    auto            canonicalPath = std::filesystem::weakly_canonical(
        fullPath.lexically_normal(), filesystemError);
    if ( !filesystemError ) {
        fullPath = canonicalPath;
    }
    return Config::pathToUtf8Generic(fullPath.lexically_normal());
}

/// @brief 将下一项控件放到当前内容区域的水平中心。
/// @param itemWidth 控件宽度。
/// @warning UI 绘制路径：只调整当前 ImGui 游标位置。
void MenuUtil::centerNextItem(float itemWidth)
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
/// @warning UI 绘制路径：只调整游标并调用统一反馈按钮。
bool MenuUtil::drawCenteredButton(const char* label, ImVec2 size)
{
    centerNextItem(size.x);
    return ::MMM::UI::FeedbackButton(label, size);
}

/// @brief 在当前内容区域内绘制自动换行文本。
/// @param text 待绘制的 UTF-8 文本。
/// @warning UI 绘制路径：只设置 ImGui 文本换行位置并绘制文本。
void MenuUtil::drawWrappedText(std::string_view text)
{
    const char* textBegin = text.empty() ? "" : text.data();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                           ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(textBegin, textBegin + text.size());
    ImGui::PopTextWrapPos();
}

/// @brief 绘制可自动换行的项目符号文本。
/// @param text 项目符号后的 UTF-8 文本。
/// @warning UI 绘制路径：只绘制 ImGui 项目符号和换行文本。
void MenuUtil::drawWrappedBulletText(std::string_view text)
{
    ImGui::Bullet();
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         ImGui::GetStyle().ItemInnerSpacing.x);
    drawWrappedText(text);
}

/// @brief 绘制标签和值，并让值在当前内容区域内自动换行。
/// @param label 标签文本。
/// @param value 值文本。
/// @warning UI 绘制路径：只绘制 ImGui 文本，不执行阻塞操作。
void MenuUtil::drawWrappedLabelValue(std::string_view label,
                                     std::string_view value)
{
    const char* labelBegin = label.empty() ? "" : label.data();
    ImGui::TextUnformatted(labelBegin, labelBegin + label.size());
    ImGui::SameLine();
    drawWrappedText(value);
}

}  // namespace MMM::UI
