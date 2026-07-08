#define IMGUI_DEFINE_MATH_OPERATORS
#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "ui/imgui/menu/MainMenuView.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <imgui.h>
#include <nfd.h>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

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

/// @brief 判断当前导出目标是否需要显示 Malody 上架 mode_ext 选项。
/// @param path 目标导出路径。
/// @return 导出 MC 且当前谱面含 Flick/折线时返回 true。
bool shouldOfferMalodyStoreModeExtForCurrentExport(const std::string& path)
{
    if ( getLowerExtension(path) != ".mc" ) return false;

    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( !session || !session->getContext().currentBeatmap ) return false;

    return hasMalodyStoreModeExtEligibleElements(
        *session->getContext().currentBeatmap);
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
/// @warning UI 绘制路径：只调整游标并调用统一反馈按钮。
bool drawCenteredButton(const char* label, ImVec2 size)
{
    centerNextItem(size.x);
    return ::MMM::UI::FeedbackButton(label, size);
}

/// @brief 在当前内容区域内绘制自动换行文本。
/// @param text 待绘制的 UTF-8 文本。
/// @warning UI 绘制路径：只设置 ImGui 文本换行位置并绘制文本。
void drawWrappedText(std::string_view text)
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
void drawWrappedBulletText(std::string_view text)
{
    ImGui::Bullet();
    ImGui::SameLine();
    drawWrappedText(text);
}

/// @brief 绘制标签和值，并让值在当前内容区域内自动换行。
/// @param label 标签文本。
/// @param value 值文本。
/// @warning UI 绘制路径：只绘制 ImGui 文本，不执行阻塞操作。
void drawWrappedLabelValue(std::string_view label, std::string_view value)
{
    const char* labelBegin = label.empty() ? "" : label.data();
    ImGui::TextUnformatted(labelBegin, labelBegin + label.size());
    ImGui::SameLine();
    drawWrappedText(value);
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
            title            = !meta.title_unicode.empty()
                                   ? meta.title_unicode
                                   : (!meta.title.empty() ? meta.title : meta.name);
            keyCount         = meta.track_count;
            version          = meta.version.empty() ? "default" : meta.version;
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
/// @param addStoreModeExtForMalodyExport 是否为 MC 导出写入上架皮肤 mode_ext。
void MainMenuView::dispatchSaveBeatmapAs(const std::string& path,
                                         bool addStoreModeExtForMalodyExport)
{
    dispatchCommand(Logic::CmdSaveBeatmapAs{
        .addStoreModeExtForMalodyExport = addStoreModeExtForMalodyExport,
        .path                           = path,
    });
}

/// @brief 直接分发当前谱面保存命令。
/// @param allowExternallyModifiedOverwrite 是否允许覆盖外部修改过的当前文件。
void MainMenuView::dispatchSaveBeatmap(bool allowExternallyModifiedOverwrite)
{
    dispatchCommand(Logic::CmdSaveBeatmap{
        .allowExternallyModifiedOverwrite = allowExternallyModifiedOverwrite,
    });
}

/// @brief 收集当前谱面导出到指定格式时需要提醒用户的兼容性问题。
/// @param path 目标导出路径。
/// @return 需要展示的警告消息列表。
std::vector<std::string> MainMenuView::collectExportCompatibilityWarnings(
    const std::string& path) const
{
    std::vector<std::string> warnings;
    const std::string        ext = getLowerExtension(path);
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

/// @brief 请求保存当前谱面，必要时先展示格式兼容性警告。
/// @param allowExternallyModifiedOverwrite 是否允许覆盖外部修改过的当前文件。
void MainMenuView::requestSaveBeatmap(bool allowExternallyModifiedOverwrite)
{
    std::string path;
    {
        auto& engine = Logic::EditorEngine::instance();
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        auto session = engine.getActiveSession();
        if ( session && session->getContext().currentBeatmap ) {
            path = Config::pathToUtf8(
                session->getContext()
                    .currentBeatmap->m_baseMapMetadata.map_path);
        }
    }

    auto warnings = collectExportCompatibilityWarnings(path);
    if ( warnings.empty() ) {
        dispatchSaveBeatmap(allowExternallyModifiedOverwrite);
        return;
    }

    m_pendingExportPath                        = std::move(path);
    m_pendingExportWarnings                    = std::move(warnings);
    m_pendingExportFormatName                  = "Malody Key";
    m_pendingCompatibilityWarningIsCurrentSave = true;
    m_pendingCompatibilityWarningAllowOverwrite =
        allowExternallyModifiedOverwrite;
    m_pendingExportShowStoreModeExtOption = false;
    m_pendingExportAddStoreModeExt        = false;
    m_showExportCompatibilityWarning      = true;
}

/// @brief 请求导出当前谱面，必要时先展示格式兼容性警告。
/// @param path 目标导出路径。
void MainMenuView::requestSaveBeatmapAs(std::string path)
{
    auto       warnings = collectExportCompatibilityWarnings(path);
    const bool showStoreModeExtOption =
        shouldOfferMalodyStoreModeExtForCurrentExport(path);
    if ( warnings.empty() && !showStoreModeExtOption ) {
        dispatchSaveBeatmapAs(path);
        return;
    }

    const std::string ext   = getLowerExtension(path);
    m_pendingExportPath     = std::move(path);
    m_pendingExportWarnings = std::move(warnings);
    m_pendingExportFormatName =
        (ext == ".osu") ? "osu!" : ((ext == ".imd") ? "RM" : "Malody Chart");
    m_pendingCompatibilityWarningIsCurrentSave  = false;
    m_pendingCompatibilityWarningAllowOverwrite = false;
    m_pendingExportShowStoreModeExtOption       = showStoreModeExtOption;
    m_pendingExportAddStoreModeExt              = Config::AppConfig::instance()
                                         .getEditorSettings()
                                         .autoAddStoreModeExtForMalodyExport;
    m_showExportCompatibilityWarning = true;
}

/// @brief 渲染导出兼容性警告弹窗。
/// @param dpiScale 当前窗口内容缩放。
void MainMenuView::renderExportCompatibilityWarningPopup(float dpiScale)
{
    constexpr const char* popupId = "谱面兼容性警告###ExportWarningModal";
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
            const char* actionText =
                m_pendingCompatibilityWarningIsCurrentSave ? "保存" : "导出";
            if ( m_pendingExportWarnings.empty() &&
                 m_pendingExportShowStoreModeExtOption ) {
                ImGui::Text("%s %s 前可以选择附加上架元数据：",
                            actionText,
                            m_pendingExportFormatName.c_str());
            } else {
                ImGui::Text("%s %s 前需要确认以下兼容性变化：",
                            actionText,
                            m_pendingExportFormatName.c_str());
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            for ( const auto& warning : m_pendingExportWarnings ) {
                drawWrappedBulletText(warning);
            }
            if ( m_pendingExportShowStoreModeExtOption ) {
                if ( !m_pendingExportWarnings.empty() ) {
                    ImGui::Spacing();
                }
                bool addStoreModeExt = m_pendingExportAddStoreModeExt;
                if ( ::MMM::UI::FeedbackCheckbox("自动添加上架皮肤 mode_ext",
                                                 &addStoreModeExt) ) {
                    m_pendingExportAddStoreModeExt = addStoreModeExt;
                    auto& settings =
                        Config::AppConfig::instance().getEditorSettings();
                    settings.autoAddStoreModeExtForMalodyExport =
                        addStoreModeExt;
                    Config::AppConfig::instance().save();
                }
                if ( ImGui::IsItemHovered() ) {
                    ImGui::SetTooltip(
                        "%s",
                        "会替换导出 MC 的 mode_ext，用于 EX Rhythm Master VI "
                        "皮肤上架提示。");
                }
            }

            ImGui::Spacing();
            drawWrappedLabelValue("目标文件：", m_pendingExportPath);
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImVec2 actionButtonSize(120.0f * dpiScale, 0.0f);
            const float  actionButtonRowWidth =
                actionButtonSize.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            centerNextItem(actionButtonRowWidth);
            const char* confirmLabel =
                m_pendingCompatibilityWarningIsCurrentSave ? "继续保存"
                                                           : "继续导出";
            if ( ::MMM::UI::FeedbackButton(confirmLabel, actionButtonSize) ) {
                if ( m_pendingCompatibilityWarningIsCurrentSave ) {
                    m_currentSaveKeyConversionWarningConfirmed = true;
                    dispatchSaveBeatmap(
                        m_pendingCompatibilityWarningAllowOverwrite);
                } else {
                    dispatchSaveBeatmapAs(m_pendingExportPath,
                                          m_pendingExportAddStoreModeExt);
                }
                m_pendingExportPath.clear();
                m_pendingExportFormatName.clear();
                m_pendingExportWarnings.clear();
                m_pendingCompatibilityWarningIsCurrentSave  = false;
                m_pendingCompatibilityWarningAllowOverwrite = false;
                m_pendingExportShowStoreModeExtOption       = false;
                m_pendingExportAddStoreModeExt              = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           actionButtonSize) ) {
                m_pendingExportPath.clear();
                m_pendingExportFormatName.clear();
                m_pendingExportWarnings.clear();
                m_pendingCompatibilityWarningIsCurrentSave  = false;
                m_pendingCompatibilityWarningAllowOverwrite = false;
                m_currentSaveKeyConversionWarningConfirmed  = false;
                m_pendingExportShowStoreModeExtOption       = false;
                m_pendingExportAddStoreModeExt              = false;
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
