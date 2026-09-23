#include "ui/imgui/menu/utils/MenuUtil.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "event/core/EventBus.h"
#include "event/logic/LogicCommandEvent.h"
#include "event/ui/menu/AudioImportTriggerEvent.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/utils/NativeFileDialog.h"
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
#include <utility>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 去除 ASCII 空白，用于解析 Malody mode 元数据。
/// @param text 原始字符串视图。
/// @return 去除首尾空白后的字符串视图。
/// @note 只处理谱面元数据允许出现的四种 ASCII 空白，不进行 Unicode 归一化。
std::string_view trimAsciiWhitespace(std::string_view text)
{
    // remove_prefix 只移动视图起点，不复制底层字符串。
    while ( !text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                              text.front() == '\n' || text.front() == '\r') ) {
        text.remove_prefix(1);
    }
    // 尾部同样在视图上收缩，返回值仍引用调用方存储。
    while ( !text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                              text.back() == '\n' || text.back() == '\r') ) {
        text.remove_suffix(1);
    }
    return text;
}

/// @brief 无异常解析整数字符串。
/// @param text 待解析文本。
/// @return 成功时返回整数，否则返回空。
/// @note 使用 from_chars 避免区域设置和异常，并要求完整文本都被消费。
std::optional<int> parseAsciiInteger(std::string_view text)
{
    // 元数据允许首尾 ASCII 空白，解析前统一裁掉。
    text = trimAsciiWhitespace(text);
    // 空白元数据不是有效 mode。
    if ( text.empty() ) return std::nullopt;

    int value = 0;
    // from_chars 不分配内存，也不会抛出异常。
    auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if ( result.ec != std::errc{} || result.ptr != text.data() + text.size() ) {
        // 范围错误或残留非数字字符均视为解析失败。
        return std::nullopt;
    }
    return value;
}

/// @brief 获取谱面当前 Malody mode 元数据，缺省时按导出器默认 slide(7) 处理。
/// @param beatMap 当前谱面。
/// @return mode 元数据有效时返回 mode；无法解析时返回空。
/// @note 缺少 MALODY 或 mode 字段时沿用导出器的 Slide 模式默认值 7。
std::optional<int> resolveMalodyModeForCompatibilityWarning(
    const BeatMap& beatMap)
{
    // 7 与 Malody Slide 模式协议值一致。
    int mode = 7;
    if ( auto it =
             beatMap.m_metadata.map_properties.find(MapMetadataType::MALODY);
         it != beatMap.m_metadata.map_properties.end() ) {
        if ( it->second.contains("mode") ) {
            // 显式 mode 必须完整解析，非法值不能静默回退以免漏警告。
            auto parsedMode = parseAsciiInteger(it->second.at("mode"));
            if ( !parsedMode ) return std::nullopt;
            mode = *parsedMode;
        }
    }
    // 返回协议整数，调用方再映射为支持的导出模式。
    return mode;
}

/// @brief 判断谱面是否包含需要上架皮肤 mode_ext 的 Malody 元素。
/// @param beatMap 当前谱面。
/// @return 含 Flick 或折线时返回 true。
/// @note 普通 Note 与 Hold 无需 mode_ext，避免显示无效上架选项。
bool hasMalodyStoreModeExtEligibleElements(const BeatMap& beatMap)
{
    return !beatMap.m_noteData.flicks.empty() ||
           !beatMap.m_noteData.polylines.empty();
}

/// @brief 从文件选择器过滤器文本中解析扩展名。
/// @param filterText 统一文件选择器当前过滤器文本。
/// @return 匹配到的扩展名；无法识别时返回空。
/// @note 同时识别扩展名和格式别名，选择文本中最先出现的候选。
std::string extensionFromFilterText(const std::string& filterText)
{
    // 过滤器文本仅含 ASCII 格式名，按 ASCII 规则统一小写。
    const std::string lower = MenuUtil::toLowerAscii(filterText);
    // 候选同时记录目标扩展名与首次匹配位置。
    struct Candidate {
        /// @brief 目标扩展名。
        std::string extension;
        /// @brief 在过滤器文本中的位置。
        /// @note npos 表示扩展名及其别名均未出现。
        size_t position{ std::string::npos };
    };

    // 先按显式扩展名初始化所有支持格式的位置。
    std::vector<Candidate> candidates = {
        { ".mmm", lower.find(".mmm") },
        { ".osu", lower.find(".osu") },
        { ".imd", lower.find(".imd") },
        { ".mc", lower.find(".mc") },
    };

    // 别名只在比现有扩展名更早时更新候选位置。
    auto updateAlias = [&](const std::string& extension,
                           const std::string& alias) {
        size_t aliasPos = lower.find(alias);
        // 未出现的别名不影响候选排序。
        if ( aliasPos == std::string::npos ) return;
        for ( auto& candidate : candidates ) {
            if ( candidate.extension == extension &&
                 aliasPos < candidate.position ) {
                // 保留同一格式所有标识中最靠前的位置。
                candidate.position = aliasPos;
            }
        }
    };
    updateAlias(".mmm", "musicmapmaker");
    updateAlias(".osu", "osu");
    updateAlias(".imd", "imd");
    updateAlias(".mc", "malody");

    // 最小位置对应当前过滤器文本最先声明的格式。
    const auto best = std::min_element(
        candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
            return a.position < b.position;
        });
    if ( best != candidates.end() && best->position != std::string::npos ) {
        // 返回规范化带点扩展名，供保存路径替换使用。
        return best->extension;
    }
    // 不认识的过滤器交由调用方保留原路径。
    return {};
}

/// @brief 替换文件名中不适合作为普通文件名的路径分隔字符。
/// @param value 原始文件名片段。
/// @return 可用于推荐文件名的文本。
/// @note 当前职责只替换路径分隔符，其余格式特有限制由后续 helper 处理。
std::string sanitizeExportFileNamePart(std::string value)
{
    // 空标题使用稳定英文兜底，保证推荐文件名非空。
    if ( value.empty() ) return "map";
    // 两个平台路径分隔符都替换为普通下划线。
    std::replace(value.begin(), value.end(), '/', '_');
    std::replace(value.begin(), value.end(), '\\', '_');
    return value;
}

/// @brief 清理 IMD 资源包的同名前缀并移除会截断首段的下划线。
/// @param value 原始歌曲名。
/// @return 可同时用作 IMD 首段、背景图和音频文件主体的名称。
/// @note 下划线是 IMD 包名字段分隔符，因此也必须替换。
std::string sanitizeImdPackagePrefix(std::string value)
{
    // 先统一处理路径分隔符并应用空值兜底。
    value = sanitizeExportFileNamePart(std::move(value));
    for ( char& character : value ) {
        // 替换 Windows 禁止字符和 IMD 自有分隔符。
        switch ( character ) {
        case '<':
        case '>':
        case ':':
        case '"':
        case '|':
        case '?':
        case '*':
        case '_': character = '-'; break;
        default: break;
        }
    }
    // 防御未来清理规则可能生成空字符串。
    return value.empty() ? "map" : value;
}

/// @brief 判断谱面是否包含 RM/IMD 无法保存的基础元数据。
/// @param meta 基础谱面元数据。
/// @return 存在不支持字段时返回 true。
/// @note 只要任一非 RM 基础字段携带有效数据就需要兼容性提醒。
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
/// @note RM 仅允许 mapLength 与 tabRows，其余来源或键都会在导出中丢失。
bool hasUnsupportedImdMapMetadata(const MapMetadata& metadata)
{
    // map_properties 按来源分组，空属性组不构成数据丢失。
    for ( const auto& [source, properties] : metadata.map_properties ) {
        if ( properties.empty() ) continue;
        // 非 RM 来源没有对应 IMD 字段，必须发出警告。
        if ( source != MapMetadataType::RM ) return true;
        for ( const auto& [key, value] : properties ) {
            (void)value;
            // RM 白名单只包含格式原生维护的两个字段。
            if ( key != "mapLength" && key != "tabRows" ) return true;
        }
    }
    return false;
}

/// @brief 判断谱面物件是否包含 RM/IMD 无法保存的额外物件元数据。
/// @param beatMap 当前谱面。
/// @return 存在不支持字段时返回 true。
/// @warning 低频导出确认路径：遍历完整物件及其元数据，不得用于逐帧 UI。
bool hasUnsupportedImdNoteMetadata(const BeatMap& beatMap)
{
    // m_allNotes 提供所有音符类型的统一引用视图。
    for ( const auto& noteRef : beatMap.m_allNotes ) {
        const auto& note = noteRef.get();
        for ( const auto& [source, properties] :
              note.m_metadata.note_properties ) {
            // 空属性组不会产生导出损失。
            if ( properties.empty() ) continue;
            // 非 RM 来源的物件元数据无法写入 IMD。
            if ( source != NoteMetadataType::RM ) return true;
            for ( const auto& [key, value] : properties ) {
                (void)value;
                // RM 物件只原生支持 Parameter 字段。
                if ( key != "Parameter" ) return true;
            }
        }
    }
    return false;
}
}  // namespace

/// @brief 发布逻辑命令事件。
/// @param cmd 需要分发给逻辑层的命令。
/// @note 命令按值封装进事件，由逻辑层统一维护事务和撤销语义。
/// @warning UI 热路径调用应只构造轻量命令，不得等待事件处理完成。
void MenuUtil::dispatchCommand(const Logic::LogicCommand& cmd)
{
    // 菜单层只发布事件，不直接访问具体命令处理器。
    Event::EventBus::instance().publish(Event::LogicCommandEvent(cmd));
}

/// @brief 唯一项目目录选择器的入口状态；仅由 UI 线程低频访问。
/// @note 选择完成后恢复 Unknown，避免后续操作继承旧来源。
static Event::ProjectOpenOrigin projectFolderPickerOrigin{
    Event::ProjectOpenOrigin::Unknown
};

/// @brief 提交项目目录选择结果并恢复选择器来源状态。
/// @param path 用户选择的项目目录。
/// @warning 仅由 UI 线程的项目选择器完成路径调用。
void MenuUtil::submitProjectFolderSelection(const std::filesystem::path& path)
{
    // 事件按值保存原生路径和打开来源。
    Event::OpenProjectEvent event;
    event.m_projectPath = path;
    event.m_origin      = projectFolderPickerOrigin;
    // 先发布完整事件，再清除只服务本次选择器的来源。
    Event::EventBus::instance().publish(event);
    projectFolderPickerOrigin = Event::ProjectOpenOrigin::Unknown;
}

/// @brief 打开项目目录选择器并发布交互开始事件。
/// @param origin 本次打开入口来源。
/// @warning 用户触发的低频路径：原生选择器可能阻塞 UI 线程。
void MenuUtil::openProjectFolderPicker(Event::ProjectOpenOrigin origin)
{
    // 来源跨越 ImGui 文件选择器的异步帧，保存在唯一 UI 状态中。
    projectFolderPickerOrigin = origin;
    // 先通知项目打开交互开始，供外层关闭冲突弹窗或跟踪来源。
    Event::ProjectOpenInteractionEvent interaction;
    interaction.m_origin = origin;
    Event::EventBus::instance().publish(interaction);
    // 引用设置读取选择器实现和最近目录。
    auto& config = Config::AppConfig::instance().getEditorSettings();
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        // 原生对话框在当前调用中完成，打开前播放统一反馈。
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t*      outPath = nullptr;
        const nfdresult_t result =
            NativeFileDialog::pickFolder(&outPath, nullptr);

        if ( result == NFD_OKAY ) {
            // NFD 返回 UTF-8 路径，转换后立即发布选择结果。
            submitProjectFolderSelection(Config::utf8ToPath(outPath));
            // NFD 分配的路径必须使用配套释放函数。
            NFD_FreePathU8(outPath);
        } else if ( result == NFD_ERROR ) {
            // 取消不是错误，只有 NFD_ERROR 写入应用日志。
            XERROR("NFD Error: {}", NFD_GetError());
        }
        return;
    }

    // ImGui 对话框跨帧保持状态，配置只允许单选目录。
    IGFD::FileDialogConfig fdConfig;
    fdConfig.path              = config.lastFilePickerPath;
    fdConfig.countSelectionMax = 1;
    fdConfig.flags             = ImGuiFileDialogFlags_Modal;
    // 记录打开前状态，避免重复请求反复播放声音。
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened("ProjectFolderPicker");
    ImGuiFileDialog::Instance()->OpenDialog(
        "ProjectFolderPicker",
        TR("ui.file_manager.open_directory").data(),
        nullptr,
        fdConfig);
    // 仅在本次调用确实由关闭变为打开时播放反馈。
    if ( !wasOpen &&
         ImGuiFileDialog::Instance()->IsOpened("ProjectFolderPicker") ) {
        ::MMM::UI::PlayPopupOpenFeedback();
    }
}

/// @brief 打开音频导入选择器并发布导入事件。
/// @warning 用户触发的低频路径：原生选择器可能阻塞。
/// @note 没有当前项目时保持无副作用退出。
void MenuUtil::openAudioImportPicker()
{
    // 音频必须导入到当前项目资源目录，故先验证项目存在。
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project ) return;

    // 选择器实现和最近路径来自编辑器设置。
    auto& config = Config::AppConfig::instance().getEditorSettings();
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        // 原生对话框同步显示，打开前播放反馈。
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t*      outPath    = nullptr;
        nfdu8filteritem_t filters[1] = { { "Audio Files",
                                           "mp3,ogg,wav,flac,opus,aac,m4a" } };
        // 过滤器覆盖应用支持导入的全部音频扩展名。
        nfdresult_t result =
            NativeFileDialog::openFile(&outPath, filters, 1, nullptr);

        if ( result == NFD_OKAY ) {
            // 事件复制 UTF-8 路径，资源导入由订阅者执行。
            Event::EventBus::instance().publish(
                Event::AudioImportTriggerEvent{ outPath });
            // 发布完成后释放 NFD 所有的路径缓冲。
            NFD_FreePathU8(outPath);
        } else if ( result == NFD_ERROR ) {
            // 用户取消静默返回，库错误才写日志。
            XERROR("NFD Error: {}", NFD_GetError());
        }
        return;
    }

    // 内置对话框跨帧显示并限制为单个只读文件名选择。
    IGFD::FileDialogConfig fdConfig;
    fdConfig.path              = config.lastFilePickerPath;
    fdConfig.countSelectionMax = 1;
    fdConfig.fileName          = "";
    fdConfig.flags             = ImGuiFileDialogFlags_Modal |
                                 ImGuiFileDialogFlags_HideColumnType |
                                 ImGuiFileDialogFlags_ReadOnlyFileNameField;
    // 记录已有打开状态，防止重复入口产生多次声音反馈。
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened("AudioImportPicker");
    ImGuiFileDialog::Instance()->OpenDialog(
        "AudioImportPicker",
        TR("ui.audio_manager.import_audio").data(),
        ".mp3,.ogg,.wav,.flac,.opus,.aac,.m4a",
        fdConfig);
    // 仅在新打开对话框时播放反馈。
    if ( !wasOpen &&
         ImGuiFileDialog::Instance()->IsOpened("AudioImportPicker") ) {
        ::MMM::UI::PlayPopupOpenFeedback();
    }
}

/// @brief 当前是否存在活跃谱面。
/// @param requireProject 是否同时要求当前项目存在。
/// @return 存在活跃谱面时返回 true。
/// @warning UI 热路径低频分支：仅在菜单展开或 action 判定时读取当前会话状态。
/// @note requireProject 为 true 时先进行无锁项目指针快速拒绝。
bool MenuUtil::hasActiveBeatmap(bool requireProject)
{
    // EditorEngine 集中管理 Project 与活动 Session。
    auto& engine = Logic::EditorEngine::instance();
    if ( requireProject && !engine.getCurrentProject() ) {
        return false;
    }

    // 逻辑线程逐轮发布存在状态，菜单判定不等待完整会话更新。
    return engine.hasActiveBeatmap();
}

/// @brief 当前是否允许触发画布编辑类快捷键。
/// @return 允许触发时返回 true。
/// @warning UI 热路径：每帧快捷键判断调用；只读取 ImGui 输入阻断状态。
/// @note 具体按键绑定由动作处理器自行判断。
bool MenuUtil::canTriggerCanvasEditingShortcut()
{
    return !ShortcutUtils::shouldBlockCanvasEditingShortcuts();
}

/// @brief 将 ASCII 字符串转换为小写。
/// @param value 原始字符串。
/// @return 转换后的字符串。
/// @note 只转换 ASCII 字节，传入 unsigned char 避免 tolower 未定义行为。
std::string MenuUtil::toLowerAscii(std::string value)
{
    // 原地转换复用调用方转移进来的字符串存储。
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

/// @brief 获取 UTF-8 路径的小写扩展名。
/// @param path UTF-8 路径字符串。
/// @return 小写扩展名。
/// @note 路径转换遵循平台原生 filesystem 规则，结果包含前导点。
std::string MenuUtil::lowerExtension(const std::string& path)
{
    // 只对 extension 结果执行 ASCII 小写，不改变目录或文件主体。
    return toLowerAscii(
        Config::pathToUtf8(Config::utf8ToPath(path).extension()));
}

/// @brief 根据导出格式生成推荐文件名。
/// @param extension 目标扩展名。
/// @param currentFileName 当前文件名，用于保留非 RM/IMD 格式的主文件名。
/// @return 推荐文件名。
/// @warning 低频文件选择路径：锁定活动 Session 并读取谱面元数据。
std::string MenuUtil::makeExportFileNameForExtension(
    const std::string& extension, const std::string& currentFileName)
{
    // Session 锁保证读取当前谱面元数据期间不会切换活动会话。
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    const BeatMap* beatMap = (session && session->getContext().currentBeatmap)
                                 ? session->getContext().currentBeatmap.get()
                                 : nullptr;

    // 缺少扩展名时默认导出项目原生 MMM 格式。
    const std::string normalizedExt =
        extension.empty() ? ".mmm" : toLowerAscii(extension);

    if ( normalizedExt == ".imd" ) {
        // IMD 文件名编码标题、键数和难度版本三个字段。
        std::string title    = "map";
        int32_t     keyCount = 0;
        std::string version  = "default";
        if ( beatMap ) {
            // 标题优先 Unicode，其次普通标题，最后内部名称。
            const auto& meta = beatMap->m_baseMapMetadata;
            title    = !meta.title_unicode.empty()
                           ? meta.title_unicode
                           : (!meta.title.empty() ? meta.title : meta.name);
            keyCount = meta.track_count;
            version  = meta.version.empty() ? "default" : meta.version;
        }
        // 各自由用户输入的字段分别清理路径分隔符。
        return fmt::format("{}_{}k_{}.imd",
                           sanitizeExportFileNamePart(title),
                           keyCount,
                           sanitizeExportFileNamePart(version));
    }

    if ( normalizedExt == ".zip" ) {
        // ZIP 表示 IMD 资源包，主体名称必须同时适配内部关联资源。
        std::string title = "map";
        if ( beatMap ) {
            const auto& meta = beatMap->m_baseMapMetadata;
            // 沿用与单文件导出一致的标题优先级。
            if ( !meta.title_unicode.empty() ) {
                title = meta.title_unicode;
            } else if ( !meta.title.empty() ) {
                title = meta.title;
            } else {
                title = meta.name;
            }
        }
        // 包前缀使用更严格清理规则，避免下划线截断首段。
        return sanitizeImdPackagePrefix(std::move(title)) + ".zip";
    }

    // 其他格式尽量保留选择器当前文件主体，仅替换扩展名。
    std::filesystem::path fileName = Config::utf8ToPath(currentFileName);
    if ( fileName.empty() ) {
        // 对话框没有文件名时从谱面内部名称生成兜底主体。
        std::string baseName = "map";
        if ( beatMap && !beatMap->m_baseMapMetadata.name.empty() ) {
            baseName = beatMap->m_baseMapMetadata.name;
        }
        // 先清理再转换为原生路径，避免标题中的分隔符改变目录层级。
        fileName = Config::utf8ToPath(sanitizeExportFileNamePart(baseName));
    }
    // replace_extension 同时处理已有扩展名和无扩展名场景。
    fileName.replace_extension(normalizedExt);
    // 只返回文件名，调用方负责保留选择器当前目录。
    return Config::pathToUtf8(fileName.filename());
}

/// @brief 按统一导出文件选择器当前格式规范化保存路径。
/// @param path 文件选择器返回的路径。
/// @return 应实际导出的目标路径。
/// @warning UI 低频路径：读取 ImGuiFileDialog 当前过滤器状态。
std::string MenuUtil::applySaveAsSelectedFormatToPath(const std::string& path)
{
    // 过滤器文本是选择器中当前激活格式的权威来源。
    std::string currentFilter = ImGuiFileDialog::Instance()->GetCurrentFilter();
    std::string currentExtension = extensionFromFilterText(currentFilter);
    if ( currentExtension.empty() ) {
        // 无法识别时保持用户输入路径，避免猜测并覆盖文件名。
        return path;
    }

    // 目录部分保持不变，只重新生成符合格式约定的文件名。
    std::filesystem::path outputPath = Config::utf8ToPath(path);
    std::string currentFileName = Config::pathToUtf8(outputPath.filename());
    std::string nextFileName =
        makeExportFileNameForExtension(currentExtension, currentFileName);
    outputPath.replace_filename(Config::utf8ToPath(nextFileName));
    // 返回统一 UTF-8 路径供事件和导出服务使用。
    return Config::pathToUtf8(outputPath);
}

/// @brief 获取另存为对话框默认打开路径。
/// @return UTF-8 编码的默认目录路径。
/// @note 优先当前项目根目录，其次最近选择器路径，最后当前目录。
std::string MenuUtil::getSaveAsPickerDefaultPath()
{
    // 项目内导出默认从项目根开始，减少误保存到外部位置。
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( project && !project->m_projectRoot.empty() ) {
        return Config::pathToUtf8(project->m_projectRoot);
    }

    // 无项目时复用持久化选择器路径，并为首次使用提供点目录。
    const auto& settings = Config::AppConfig::instance().getEditorSettings();
    return settings.lastFilePickerPath.empty() ? std::string(".")
                                               : settings.lastFilePickerPath;
}

/// @brief 收集当前谱面导出到指定格式时需要提醒用户的兼容性问题。
/// @param path 目标导出路径。
/// @param malodyExportMode MC 导出时显式选择的模式；为空则读取谱面元数据。
/// @return 需要展示的警告消息列表。
/// @warning 低频导出确认路径：锁定 Session 并遍历完整 timing 与部分元数据。
std::vector<std::string> MenuUtil::collectExportCompatibilityWarnings(
    const std::string& path, std::optional<MMM::MalodyMode> malodyExportMode)
{
    std::vector<std::string> warnings;
    // 扩展名决定目标格式能力集合。
    const std::string ext = lowerExtension(path);
    // 原生 MMM 与未知格式没有本 helper 定义的兼容性警告。
    if ( ext != ".osu" && ext != ".imd" && ext != ".mc" ) return warnings;

    // 锁住活动 Session，保证整个兼容性快照来自同一谱面版本。
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( !session || !session->getContext().currentBeatmap ) return warnings;

    // 谱面引用只在当前锁区间内使用。
    const BeatMap& beatMap = *session->getContext().currentBeatmap;

    // 先收集跨格式共用的特性标志，后续按目标格式生成文案。
    bool hasJumpOrHsTiming        = false;
    bool hasNegativeScrollTiming  = false;
    bool hasAnyNonBpmTimingForImd = false;
    bool hasFlick                 = !beatMap.m_noteData.flicks.empty();
    bool hasPolyline              = !beatMap.m_noteData.polylines.empty();
    bool hasUnsupportedBaseMeta   = false;
    bool hasUnsupportedMapMeta    = false;
    bool hasUnsupportedNoteMeta   = false;

    // Timing 只遍历一次，同时满足 osu 与 IMD 的能力判断。
    for ( const auto& timing : beatMap.m_timings ) {
        if ( timing.m_timingEffect == TimingEffect::JUMP ||
             timing.m_timingEffect == TimingEffect::HS ) {
            hasJumpOrHsTiming = true;
            // IMD 除 BPM 外不保存任何 timing 效果。
            hasAnyNonBpmTimingForImd = true;
        } else if ( timing.m_timingEffect == TimingEffect::SCROLL ) {
            hasAnyNonBpmTimingForImd = true;
            if ( timing.m_timingEffectParameter < 0.0 ) {
                // osu 只需单独警告负 Scroll，正值可由导出器处理。
                hasNegativeScrollTiming = true;
            }
        }
    }

    if ( ext == ".osu" ) {
        // osu 警告分别覆盖 timing 与物件降级，便于用户逐项判断。
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
            // 折线展开会同时忽略子 Flick 并保留子 Hold，需明确说明。
            warnings.push_back(
                "Polyline 物件会在 osu! 导出中展开：其中 Flick "
                "子物件会被忽略，只导出其中所有 Hold。");
        }
    } else if ( ext == ".imd" ) {
        // IMD 元数据检查按基础、谱面扩展和物件扩展三个层级执行。
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
            // 基础和谱面扩展共同使用一条格式能力摘要，避免重复警告。
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
        // 显式对话框选项优先于谱面原有 mode 元数据。
        const auto mode =
            malodyExportMode
                ? std::optional<int>(malodyModeValue(*malodyExportMode))
                : resolveMalodyModeForCompatibilityWarning(beatMap);
        if ( mode && *mode == 0 && (hasFlick || hasPolyline) ) {
            // Key 模式不能原样表达扩展物件，必须在覆盖目标前确认。
            warnings.push_back(
                "Malody key(0) 模式无法存储 Flick/折线；继续保存会将 "
                "Flick 作为单 Note 写出，忽略 Polyline 中所有 subFlick，"
                "并将所有 subHold 作为普通 Hold "
                "写出，转换结果会覆盖目标谱面。");
        }
    }

    // 文案顺序固定，调用方按顺序绘制项目符号列表。
    return warnings;
}

/// @brief 获取当前谱面的 Malody 导出模式，缺省或无效时使用 Slide。
/// @return 当前可用于导出选项的 Malody 模式。
/// @warning 低频导出窗口路径：锁定活动 Session 读取元数据。
MMM::MalodyMode MenuUtil::currentMalodyExportMode()
{
    // 整个解析期间保持活动谱面稳定。
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( !session || !session->getContext().currentBeatmap ) {
        // 没有谱面上下文时使用最能保留扩展物件的 Slide 模式。
        return MalodyMode::Slide;
    }

    const auto mode = resolveMalodyModeForCompatibilityWarning(
        *session->getContext().currentBeatmap);
    if ( mode && *mode == malodyModeValue(MalodyMode::Key) ) {
        // 只有明确协议值 0 映射为 Key，其余或非法值统一回退 Slide。
        return MalodyMode::Key;
    }
    return MalodyMode::Slide;
}

/// @brief 判断当前 MC 导出目标是否需要显示上架 mode_ext 选项。
/// @param path 目标导出路径。
/// @return 导出 MC 且当前谱面含 Flick/折线时返回 true。
/// @warning 低频导出窗口路径：锁定活动 Session 读取物件集合。
bool MenuUtil::shouldOfferMalodyStoreModeExtForCurrentExport(
    const std::string& path)
{
    // 非 MC 导出不展示 Malody 专用上架选项。
    if ( lowerExtension(path) != ".mc" ) return false;

    // Session 锁保证元素集合判断期间活动谱面稳定。
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> sessionLock(engine.getSessionMutex());
    auto                                  session = engine.getActiveSession();
    if ( !session || !session->getContext().currentBeatmap ) return false;

    // 只有存在需要扩展模式表达的物件时提供选项。
    return hasMalodyStoreModeExtEligibleElements(
        *session->getContext().currentBeatmap);
}

/// @brief 将项目谱面路径规范化为候选比较键。
/// @param projectRoot 当前项目根目录。
/// @param path 谱面路径，可为项目相对路径或绝对路径。
/// @return 规范化后的 UTF-8 路径键。
/// @warning 低频候选收集路径：weakly_canonical 可能访问文件系统。
std::string MenuUtil::makeProjectBeatmapPathKey(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    // 空路径没有稳定身份，直接返回空键。
    if ( path.empty() ) return {};

    // 相对路径以当前项目根解析，绝对路径保持自身根。
    std::filesystem::path fullPath =
        path.is_absolute() ? path : (projectRoot / path);
    std::error_code filesystemError;
    // 使用无异常重载解析现有前缀中的符号链接和点段。
    auto canonicalPath = std::filesystem::weakly_canonical(
        fullPath.lexically_normal(), filesystemError);
    if ( !filesystemError ) {
        // 规范化成功时优先使用文件系统感知路径。
        fullPath = canonicalPath;
    }
    // 失败时仍返回词法规范化键，保证不存在文件也可稳定比较。
    return Config::pathToUtf8Generic(fullPath.lexically_normal());
}

/// @brief 将下一项控件放到当前内容区域的水平中心。
/// @param itemWidth 控件宽度。
/// @warning UI 绘制路径：只调整当前 ImGui 游标位置。
/// @note 内容宽度不足时保持原位置，避免产生负偏移。
void MenuUtil::centerNextItem(float itemWidth)
{
    // 可用宽度由当前窗口内容区域和已提交布局共同决定。
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    if ( availableWidth > itemWidth ) {
        // 仅增加当前 X 坐标，保留调用方既有左侧布局偏移。
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             (availableWidth - itemWidth) * 0.5f);
    }
}

/// @brief 绘制水平居中的按钮。
/// @param label 按钮文本和 ImGui ID。
/// @param size 按钮尺寸。
/// @return 按钮被点击时返回 true。
/// @warning UI 绘制路径：只调整游标并调用统一反馈按钮。
/// @note size.x 应是调用方已按 DPI 计算的目标宽度。
bool MenuUtil::drawCenteredButton(const char* label, ImVec2 size)
{
    // 先移动下一项起点，再使用统一按钮反馈入口。
    centerNextItem(size.x);
    return ::MMM::UI::FeedbackButton(label, size);
}

/// @brief 在当前内容区域内绘制自动换行文本。
/// @param text 待绘制的 UTF-8 文本。
/// @warning UI 绘制路径：只设置 ImGui 文本换行位置并绘制文本。
/// @note 显式结束指针保证 string_view 无需空字符结尾。
void MenuUtil::drawWrappedText(std::string_view text)
{
    // 空视图使用有效空字符串指针，避免对 nullptr 做指针运算。
    const char* textBegin = text.empty() ? "" : text.data();
    // 换行边界设置到当前内容区域右侧。
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() +
                           ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(textBegin, textBegin + text.size());
    // 恢复调用方换行栈。
    ImGui::PopTextWrapPos();
}

/// @brief 绘制可自动换行的项目符号文本。
/// @param text 项目符号后的 UTF-8 文本。
/// @warning UI 绘制路径：只绘制 ImGui 项目符号和换行文本。
/// @note 文本起点额外使用主题内部间距，避免紧贴项目符号。
void MenuUtil::drawWrappedBulletText(std::string_view text)
{
    // Bullet 提交独立项目符号并推进当前行布局。
    ImGui::Bullet();
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         ImGui::GetStyle().ItemInnerSpacing.x);
    // 复用统一换行文本实现，保持 string_view 边界安全。
    drawWrappedText(text);
}

/// @brief 绘制标签和值，并让值在当前内容区域内自动换行。
/// @param label 标签文本。
/// @param value 值文本。
/// @warning UI 绘制路径：只绘制 ImGui 文本，不执行阻塞操作。
/// @note 标签保持单行，值从同行当前位置开始按剩余宽度换行。
void MenuUtil::drawWrappedLabelValue(std::string_view label,
                                     std::string_view value)
{
    // 与正文 helper 一致地为可能为空的标签提供有效指针。
    const char* labelBegin = label.empty() ? "" : label.data();
    ImGui::TextUnformatted(labelBegin, labelBegin + label.size());
    // 值紧接标签绘制，具体换行边界由 drawWrappedText 计算。
    ImGui::SameLine();
    drawWrappedText(value);
}

}  // namespace MMM::UI
