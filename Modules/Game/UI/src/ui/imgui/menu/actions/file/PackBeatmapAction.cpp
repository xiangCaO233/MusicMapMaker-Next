#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"

#include "common/LogicCommands.h"
#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectResourceService.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/PackageFileTypes.h"
#include "mmm/project/Project.h"
#include "ui/imgui/menu/package/PackageDefaultSelection.h"
#include "ui/imgui/menu/package/PackageDialogState.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/imgui/status/IStatusMessageSink.h"
#include "ui/utils/NativeFileDialog.h"
#include "ui/utils/UIWidgetUtils.h"
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <imgui.h>
#include <nfd.h>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace MMM::UI
{
namespace
{
/// @brief 将 ASCII 字符串转换为小写，用于扩展名判断。
/// @param value 待转换文本。
/// @return 仅 ASCII 大写字母被转换后的文本。
/// @note 扩展名协议为 ASCII，非 ASCII 字节保持原值。
std::string toLowerAscii(std::string value)
{
    // cctype 要求参数可表示为 unsigned char，避免有符号 char 未定义行为。
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

/// @brief 将下一项控件放到当前内容区域的水平中心。
/// @param itemWidth 控件宽度。
/// @warning UI 绘制路径：只调整当前 ImGui 游标位置。
/// @note 可用宽度不足时保持当前左对齐位置，避免产生负偏移。
void centerNextItem(float itemWidth)
{
    // 内容区域宽度已扣除窗口内边距和滚动条占用。
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    if ( availableWidth > itemWidth ) {
        // 只移动剩余空间的一半，使指定宽度控件水平居中。
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             (availableWidth - itemWidth) * 0.5f);
    }
}

/// @brief 绘制水平居中的按钮。
/// @param label 按钮文本和 ImGui ID。
/// @param size 按钮尺寸。
/// @return 按钮被点击时返回 true。
/// @warning UI 绘制路径：只调整游标并调用统一反馈按钮。
/// @note 统一反馈入口负责悬停与点击音效，本函数只封装布局。
bool drawCenteredButton(const char* label, ImVec2 size)
{
    // 必须在绘制按钮前调整游标，否则测量将作用于后续控件。
    centerNextItem(size.x);
    return ::MMM::UI::FeedbackButton(label, size);
}

/// @brief 估算 Checkbox 绘制指定标签时需要的宽度。
/// @param label Checkbox 标签文本。
/// @return 当前字体和样式下的控件宽度。
/// @warning UI 绘制路径：只读取当前 ImGui 样式和字体测量结果。
/// @note 结果用于 SameLine 容量判断，不参与实际 Checkbox 尺寸设置。
float getCheckboxDisplayWidth(const char* label)
{
    // FrameHeight 覆盖方框宽度，标签另加内部间距。
    const ImGuiStyle& style     = ImGui::GetStyle();
    const ImVec2      labelSize = ImGui::CalcTextSize(label, nullptr, true);
    float             width     = ImGui::GetFrameHeight();
    if ( labelSize.x > 0.0f ) {
        // 隐藏 ID 后缀不计入 CalcTextSize 的显示宽度。
        width += style.ItemInnerSpacing.x + labelSize.x;
    }
    return width;
}

/// @brief 若下一控件还能放进当前行，则把光标移动到同一行。
/// @param nextItemWidth 下一控件预计宽度。
/// @warning UI 绘制路径：只根据上一控件位置和内容边界决定是否调用 SameLine。
/// @note 控件放不下时自然换行，避免窄窗口横向裁切。
void sameLineIfItemFits(float nextItemWidth)
{
    // nextItemX 使用屏幕坐标，与窗口内容最大边界保持同一坐标系。
    const ImGuiStyle& style = ImGui::GetStyle();
    const float nextItemX   = ImGui::GetItemRectMax().x + style.ItemSpacing.x;
    const float contentMaxX =
        ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
    if ( nextItemX + nextItemWidth <= contentMaxX ) {
        // 只有完整宽度可容纳时才保留在当前行。
        ImGui::SameLine();
    }
}

/// @brief 替换文件名中不适合作为普通文件名的路径分隔字符。
/// @param value 原始文件名片段。
/// @return 不包含正反斜杠的文件名片段。
/// @note 此处只阻止片段意外形成子目录，扩展名由调用方追加。
std::string sanitizePackageFileNamePart(std::string value)
{
    // 空项目目录名使用稳定兜底，保证保存对话框始终有默认文件名。
    if ( value.empty() ) return "map";
    // 同时替换 POSIX 与 Windows 路径分隔符，保持跨平台一致。
    std::replace(value.begin(), value.end(), '/', '_');
    std::replace(value.begin(), value.end(), '\\', '_');
    return value;
}

/// @brief 获取打包输出对话框默认打开路径，优先使用当前项目根目录。
/// @param settings 编辑器设置，用于无项目时回退到通用文件选择器路径。
/// @return UTF-8 编码的默认目录路径。
/// @note 只读取当前项目指针，不延长项目生命周期。
std::string getPackagePickerDefaultPath(const Config::EditorSettings& settings)
{
    // 已打开项目时优先在项目根目录旁生成包，便于用户定位。
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( project && !project->m_projectRoot.empty() ) {
        return Config::pathToUtf8(project->m_projectRoot);
    }

    // 无项目路径时沿用通用选择器最近目录，首次使用回退当前目录。
    return settings.lastFilePickerPath.empty() ? std::string(".")
                                               : settings.lastFilePickerPath;
}

/// @brief 获取打包格式显示名称。
/// @param type 打包格式。
/// @return 用户界面显示的格式名称。
/// @note 扩展名来自统一格式规则，避免显示文案与实际输出不一致。
std::string getPackageTypeDisplayName(PackageFileType type)
{
    // 格式规则对象的生命周期由静态注册表保证。
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
    // 防御无效枚举值，界面仍展示项目原生包格式。
    return "MusicMapMaker Package (.mpk)";
}

/// @brief 取得打包格式扩展名。
/// @param type 打包格式。
/// @return 带前导点的扩展名。
/// @note 返回副本便于后续 replace_extension 使用。
std::string getPackageExtension(PackageFileType type)
{
    return std::string(getPackageSupportedFileTypes(type).m_packageExtension);
}

/// @brief 取得打包格式要求的谱面文件扩展名。
/// @param type 打包格式。
/// @return 带前导点的谱面扩展名。
/// @note 多种目标谱面格式时首项是打包转换器的首选输出格式。
std::string getPackageBeatmapExtension(PackageFileType type)
{
    // 空规则仅作为防御分支，原生 .mmm 能保留完整项目语义。
    const auto& types = getPackageSupportedFileTypes(type);
    if ( types.m_beatmapExtensions.empty() ) return ".mmm";
    return std::string(types.m_beatmapExtensions.front());
}

/// @brief 判断打包格式是否需要显示保存转换谱面选项。
/// @param type 打包格式。
/// @return 需要显示时返回 true。
/// @note MPK 原生保留谱面源，无需把转换产物写回项目。
bool shouldShowConvertedBeatmapSaveOption(PackageFileType type)
{
    return type == PackageFileType::Mcz || type == PackageFileType::Osz;
}

/// @brief 判断谱面源是否需要转换为当前目标包的谱面格式。
/// @param type 目标打包格式。
/// @param relativePath 项目相对谱面路径。
/// @return 谱面源需要转换时返回 true。
/// @note 必须同时是可读源格式且不是目标包可直接存储的谱面格式。
bool shouldConvertPackageCandidateBeatmap(PackageFileType    type,
                                          const std::string& relativePath)
{
    // 先词法规范化路径再取扩展名，统一点段和平台分隔符。
    const auto  extension = toLowerAscii(Config::pathToUtf8(
        Config::utf8ToPath(relativePath).lexically_normal().extension()));
    const auto& types     = getPackageSupportedFileTypes(type);
    // 源支持性负责可解析条件，资源支持性负责是否需要转码。
    return isPackageBeatmapSourceExtensionSupported(types, extension) &&
           !isPackageResourceExtensionSupported(
               types, PackageResourceType::Beatmap, extension);
}

/// @brief 判断当前已选候选中是否存在需要转换的谱面源。
/// @param type 目标打包格式。
/// @param candidateFiles 当前候选文件缓存。
/// @return 存在需要转换的已选谱面源时返回 true。
/// @warning UI 热路径：打包选择弹窗可见时每帧查询；只遍历候选缓存。
/// @note 未选中或非谱面资源不会影响保存转换谱面的选项状态。
bool hasSelectedPackageBeatmapSourceRequiringConversion(
    PackageFileType                          type,
    const std::vector<PackageCandidateFile>& candidateFiles)
{
    // any_of 在首个需要转换的已选谱面处短路。
    return std::any_of(candidateFiles.begin(),
                       candidateFiles.end(),
                       [&](const PackageCandidateFile& file) {
                           return file.selected &&
                                  file.resourceType ==
                                      PackageResourceType::Beatmap &&
                                  shouldConvertPackageCandidateBeatmap(
                                      type, file.relativePath);
                       });
}

/// @brief 判断打包格式是否需要显示旧 IMD 兼容谱面选项。
/// @param type 打包格式。
/// @return 需要显示时返回 true。
/// @note 旧 IMD 兼容副本仅属于 Malody MCZ 输出语义。
bool shouldShowLegacyImdPackageOption(PackageFileType type)
{
    return type == PackageFileType::Mcz;
}

/// @brief 判断打包转换前是否需要让用户补充目标谱面元数据。
/// @param type 目标打包格式。
/// @param extension 来源文件扩展名。
/// @return 需要补充元数据时返回 true。
/// @note 仅转换时无法从源格式完整推导目标必填字段才打开补充窗口。
bool shouldPreparePackageBeatmapMetadataEdit(PackageFileType    type,
                                             const std::string& extension)
{
    // MCZ 的 IMD 和 OSZ 的非 osu/mmm 通用源需要人工确认目标字段。
    switch ( type ) {
    case PackageFileType::Mcz: return packageExtensionEquals(extension, ".imd");
    case PackageFileType::Osz:
        return packageExtensionInList(PACKAGE_BEATMAP_SOURCE_EXTENSIONS,
                                      extension) &&
               !packageExtensionEquals(extension, ".osu") &&
               !packageExtensionEquals(extension, ".mmm");
    case PackageFileType::Mpk: return false;
    }
    // 防御未来枚举扩展时默认不阻塞打包流程。
    return false;
}

/// @brief 构建打包元数据补充窗口的提示文本。
/// @param type 目标打包格式。
/// @return 用户界面提示文本。
/// @note 提示包含实际目标谱面扩展名，帮助用户理解转换结果。
std::string makePackageMetadataEditPrompt(PackageFileType type)
{
    // 扩展名由当前格式规则决定，不在文案中重复硬编码。
    const std::string targetExtension = getPackageBeatmapExtension(type);
    switch ( type ) {
    case PackageFileType::Mcz:
        return "这些谱面将转换为 " + targetExtension +
               "。请补充 Malody 包需要的元数据：";
    case PackageFileType::Osz:
        return "这些谱面将转换为 " + targetExtension +
               "。请补充 osu! 包需要的元数据：";
    case PackageFileType::Mpk:
        return "这些谱面将转换为 " + targetExtension +
               "。请补充目标格式需要的元数据：";
    }
    // 无效枚举或未来格式使用中性描述。
    return "这些谱面将转换为 " + targetExtension +
           "。请补充目标格式需要的元数据：";
}

/// @brief 取得原生文件选择器使用的扩展名过滤器。
/// @param type 打包格式。
/// @return 不带前导点的扩展名。
/// @note NFD 过滤器协议要求裸扩展名，与统一选择器格式不同。
const char* getNativePackageOutputFilterText(PackageFileType type)
{
    switch ( type ) {
    case PackageFileType::Mcz: return "mcz";
    case PackageFileType::Osz: return "osz";
    case PackageFileType::Mpk: return "mpk";
    }
    // 未知类型按原生包处理，和显示名回退保持一致。
    return "mpk";
}

/// @brief 取得统一文件选择器使用的扩展名过滤器。
/// @param type 打包格式。
/// @return 带前导点的扩展名。
/// @note ImGuiFileDialog 过滤器使用带点扩展名。
const char* getUnifiedPackageOutputFilterText(PackageFileType type)
{
    switch ( type ) {
    case PackageFileType::Mcz: return ".mcz";
    case PackageFileType::Osz: return ".osz";
    case PackageFileType::Mpk: return ".mpk";
    }
    // 未知类型按原生包处理，避免构造无过滤器对话框。
    return ".mpk";
}

/// @brief 判断扩展名是否为打包产物扩展名。
/// @param extension 待检查扩展名。
/// @return 是否应从候选资源列表中排除。
/// @note 排除现有包和普通 ZIP，防止包中递归嵌套历史导出产物。
bool isPackageArchiveExtension(const std::string& extension)
{
    // 统一注册表覆盖 MCZ、OSZ、MPK，ZIP 需额外显式排除。
    return findPackageSupportedFileTypes(extension) != nullptr ||
           packageExtensionEquals(extension, ".zip");
}

/// @brief 将文本写入固定大小输入缓存。
/// @param buffer 目标输入缓存。
/// @param text 源文本。
/// @tparam N 固定缓存容量，必须至少为一以容纳终止符。
/// @note 超长文本按字节截断；缓存预清零保证仍是合法 C 字符串。
template<std::size_t N>
void copyToPackageInputBuffer(std::array<char, N>& buffer,
                              std::string_view     text)
{
    // 清零同时移除上一次较长输入留下的尾部内容。
    buffer.fill('\0');
    // 最后一字节保留给 NUL，调用处使用的缓冲区容量均大于零。
    const std::size_t count = std::min(text.size(), buffer.size() - 1);
    // 只复制有效文本，不把 string_view 之后的存储带入缓冲区。
    std::copy_n(text.begin(), count, buffer.begin());
}

/// @brief 从固定大小输入缓存读取文本。
/// @param buffer 输入缓存。
/// @return 缓存中的 C 字符串文本。
/// @tparam N 固定缓存容量。
/// @pre buffer 必须由清零初始化或 copyToPackageInputBuffer 维护终止符。
template<std::size_t N>
std::string packageInputBufferText(const std::array<char, N>& buffer)
{
    // 输入控件保证 NUL 结尾，因此按 C 字符串恢复实际编辑长度。
    return std::string(buffer.data());
}

/// @brief 根据扩展名推断资源分类显示文本。
/// @param types 当前打包格式规则。
/// @param extension 待检查扩展名。
/// @return 资源分类显示文本，空字符串表示不符合规则。
/// @note 谱面源优先判断，防止通用资源扩展规则覆盖转换入口。
/// @details 当格式允许任意已知媒体格式时，使用项目级已知扩展集合；否则
/// 严格使用目标包规则的资源白名单。返回空值意味着目录扫描应忽略该文件。
std::string getPackageCandidateTypeLabel(const PackageSupportedFileTypes& types,
                                         const std::string& extension)
{
    if ( isPackageBeatmapSourceExtensionSupported(types, extension) ) {
        // 可解析但目标包不能直接存储时，在 UI 中明确标记为谱面源。
        if ( !isPackageResourceExtensionSupported(
                 types, PackageResourceType::Beatmap, extension) ) {
            return "谱面源";
        }
        // 目标格式原生支持该扩展名，可直接作为谱面资源写包。
        return "谱面";
    }
    // allowAll 标志放宽到项目已知格式，否则严格采用目标格式白名单。
    if ( types.m_allowAllAudioFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Audio,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Audio, extension) ) {
        return "音频";
    }
    // 视频和图片遵循与音频相同的宽松或严格匹配策略。
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
    // 空标签使目录扫描阶段忽略未知或不支持文件。
    return {};
}

/// @brief 根据扩展名推断资源分类。
/// @param types 当前打包格式规则。
/// @param extension 待检查扩展名。
/// @return 可打包资源分类；不符合规则时返回空。
/// @note 判定顺序必须与 getPackageCandidateTypeLabel 保持一致。
/// @details 该枚举结果驱动谱面解析、依赖锁定和 UI 行为，因此不能只根据
/// 显示标签反向推导。转换前的谱面源仍归入 Beatmap 类别。
std::optional<PackageResourceType> getPackageCandidateResourceType(
    const PackageSupportedFileTypes& types, const std::string& extension)
{
    if ( isPackageBeatmapSourceExtensionSupported(types, extension) ) {
        // 转换源仍归类为 Beatmap，以便后续读取依赖和元数据。
        return PackageResourceType::Beatmap;
    }
    if ( types.m_allowAllAudioFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Audio,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Audio, extension) ) {
        return PackageResourceType::Audio;
    }
    if ( types.m_allowAllVideoFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Video,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Video, extension) ) {
        return PackageResourceType::Video;
    }
    if ( types.m_allowAllImageFormats
             ? isKnownPackageResourceExtension(PackageResourceType::Image,
                                               extension)
             : isPackageResourceExtensionSupported(
                   types, PackageResourceType::Image, extension) ) {
        return PackageResourceType::Image;
    }
    // 未知扩展名不进入候选列表，也不会被依赖锁定。
    return std::nullopt;
}

/// @brief 规范化项目相对路径为 UTF-8 通用分隔符形式。
/// @param path 项目相对路径。
/// @return 规范化后的路径。
/// @note 输出统一使用正斜杠，作为候选索引和序列化比较键。
std::string normalizePackageRelativeUtf8(const std::string& path)
{
    // 空引用保持为空，防止词法规范化把它转换为当前目录点号。
    if ( path.empty() ) return {};
    // 本机 path 负责折叠点段，generic UTF-8 负责跨平台稳定表示。
    return Config::pathToUtf8Generic(
        Config::utf8ToPath(path).lexically_normal());
}

/// @brief 判断项目相对路径是否会逃逸项目根目录。
/// @param path 项目相对路径。
/// @return 路径逃逸根目录时返回 true。
/// @note 只做词法检查，不访问磁盘或解析符号链接。
bool packageDependencyPathEscapesRoot(const std::filesystem::path& path)
{
    // 规范化后残留的父目录分量意味着路径会越过项目根。
    const auto normalized = path.lexically_normal();
    for ( const auto& part : normalized ) {
        if ( part == ".." ) return true;
    }
    // 绝对路径即使不含父目录分量也不属于项目相对依赖。
    return normalized.is_absolute();
}

/// @brief 解析项目相对路径为文件系统路径。
/// @param projectRoot 项目根目录。
/// @param path 项目相对或绝对路径。
/// @return 规范化后的文件系统路径。
/// @note 绝对输入保持根目录，只有相对输入才拼接 projectRoot。
std::filesystem::path resolvePackageProjectPath(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    if ( path.empty() || path.is_absolute() ) {
        // 空路径交由调用方判断，绝对路径只折叠词法点段。
        return path.lexically_normal();
    }
    // 拼接后规范化，避免候选比较受 ./ 片段影响。
    return (projectRoot / path).lexically_normal();
}

/// @brief 将文件系统路径转为项目相对路径。
/// @param projectRoot 项目根目录。
/// @param path 文件系统路径。
/// @return 项目相对路径；无法转换时返回空。
/// @note 任何逃逸根目录的结果都被拒绝，保证包依赖属于当前项目。
std::filesystem::path makePackageProjectRelativePath(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    // 空路径不表示项目根目录，直接作为无依赖处理。
    if ( path.empty() ) return {};
    if ( path.is_relative() ) {
        // 已相对的输入仍需折叠并检查父目录逃逸。
        const auto relativePath = path.lexically_normal();
        return packageDependencyPathEscapesRoot(relativePath)
                   ? std::filesystem::path{}
                   : relativePath;
    }

    // 绝对输入先获得项目根绝对表示，再计算跨平台相对路径。
    std::error_code filesystemError;
    const auto root = std::filesystem::absolute(projectRoot, filesystemError);
    // 使用 error_code 保持项目禁异常约束。
    if ( filesystemError ) return {};

    auto relativePath = std::filesystem::relative(path, root, filesystemError);
    // 不同文件系统根或空结果不能形成有效包内路径。
    if ( filesystemError || relativePath.empty() ) return {};
    relativePath = relativePath.lexically_normal();
    return packageDependencyPathEscapesRoot(relativePath)
               ? std::filesystem::path{}
               : relativePath;
}

/// @brief 将文件系统路径转为 UTF-8 项目相对路径。
/// @param projectRoot 项目根目录。
/// @param path 文件系统路径。
/// @return UTF-8 项目相对路径；无法转换时返回空。
/// @note 该函数是文件系统边界到包内通用路径协议的统一入口。
std::string makePackageProjectRelativeUtf8(
    const std::filesystem::path& projectRoot, const std::filesystem::path& path)
{
    // 先复用逃逸检查，再进行 UTF-8 通用分隔符转换。
    const auto relativePath = makePackageProjectRelativePath(projectRoot, path);
    if ( relativePath.empty() ) return {};
    return Config::pathToUtf8Generic(relativePath);
}

/// @brief 在项目根目录和谱面目录之间解析谱面 metadata 资源路径。
/// @param projectRoot 项目根目录。
/// @param mapDirectory 谱面文件所在目录。
/// @param path metadata 中记录的资源路径。
/// @param preferProjectRoot 是否优先按项目根目录解析。
/// @return 可访问优先的资源路径。
/// @note 原生 MMM 元数据通常相对项目根，外部谱面格式通常相对谱面目录。
/// @details 两个候选都不存在时仍返回格式对应的首选路径，使后续项目相对
/// 转换能够保留预期缺失位置，而不是丢失依赖诊断上下文。
std::filesystem::path resolvePackageMetadataResourcePath(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& mapDirectory,
    const std::filesystem::path& path, bool preferProjectRoot)
{
    if ( path.empty() || path.is_absolute() ) {
        // 绝对资源不重新挂接项目根，空值留给调用方忽略。
        return path.lexically_normal();
    }

    // 同时构造两种历史语义的候选路径，再按来源格式决定优先级。
    const auto projectPath = resolvePackageProjectPath(projectRoot, path);
    const auto mapPath     = (mapDirectory / path).lexically_normal();

    // 存在性查询使用 error_code，单个候选失败后清理再尝试另一位置。
    std::error_code filesystemError;
    if ( preferProjectRoot ) {
        if ( std::filesystem::exists(projectPath, filesystemError) ) {
            return projectPath;
        }
        // 项目根候选不存在时兼容相对谱面目录保存的旧数据。
        filesystemError.clear();
        if ( std::filesystem::exists(mapPath, filesystemError) ) {
            return mapPath;
        }
        // 两者均不存在时返回首选路径，后续缺失依赖诊断仍能显示它。
        return projectPath;
    }

    // 外部格式优先谱面同目录，再回退项目根目录。
    if ( std::filesystem::exists(mapPath, filesystemError) ) return mapPath;
    filesystemError.clear();
    if ( std::filesystem::exists(projectPath, filesystemError) ) {
        return projectPath;
    }
    // 均不存在时保留格式语义下的首选路径供诊断。
    return mapPath;
}

/// @brief 向依赖列表追加一个去重后的项目相对路径。
/// @param dependencies 依赖路径列表。
/// @param relativePath 项目相对路径。
/// @note 保持首次发现顺序，同时阻止多字段引用生成重复候选锁定。
void appendUniquePackageDependency(std::vector<std::string>& dependencies,
                                   const std::string&        relativePath)
{
    // 无法解析为项目相对路径的依赖由其他诊断列表处理。
    if ( relativePath.empty() ) return;
    if ( std::find(dependencies.begin(), dependencies.end(), relativePath) !=
         dependencies.end() ) {
        // 线性查重适合单谱面小规模依赖，并保持容器结构简单。
        return;
    }
    // 新依赖追加到末尾，便于提示按元数据扫描顺序展示。
    dependencies.push_back(relativePath);
}

/// @brief 谱面打包扫描结果。
/// @details 聚合依赖路径、缺失引用和目标格式兼容性标志，供候选 UI 使用。
/// @note 该值对象不保存 BeatMap 或 Project 引用，可安全移动进候选状态。
struct PackageBeatmapInfo {
    /// @brief 谱面引用的资源路径列表。
    /// @note 路径均规范化为项目相对 UTF-8 通用形式且已去重。
    std::vector<std::string> dependencyRelativePaths;

    /// @brief 无法解析为项目资源的谱面音频引用。
    /// @note 每项包含字段名和原始引用，直接用于缺失依赖提示。
    std::vector<std::string> unresolvedAudioReferences;

    /// @brief 谱面文件是否读取失败。
    /// @note 读取失败会阻止所选候选进入打包阶段。
    bool loadFailed{ false };

    /// @brief 是否包含 Flick 或折线。
    /// @note 仅 MCZ Slide 上架 mode_ext 与 Key 兼容警告消费该标志。
    bool hasStoreModeExtEligibleElements{ false };

    /// @brief 默认 Main 音频是否使用非 OGG 文件。
    /// @note 决定 MCZ 原点对齐兼容选项是否可用。
    bool hasNonOggMainAudio{ false };
};

/// @brief 等待按单谱面批量解析的音频引用。
/// @details 将解析键与诊断来源绑定，批量查询后仍可生成明确错误文本。
/// @note 容器顺序与解析服务返回顺序构成一一对应关系，不得单独排序。
struct PackageAudioReference {
    /// @brief 谱面中保存的音频资源 ID 或路径。
    /// @note 交给 ProjectResourceService 按项目兼容规则解析。
    std::string m_audioReference;

    /// @brief 缺失依赖诊断使用的引用字段名称。
    /// @note 不参与查找，仅用于用户提示定位数据来源。
    std::string m_fieldLabel;
};

/// @brief 将单个 metadata 资源路径解析并追加到依赖列表。
/// @param dependencies 依赖路径列表。
/// @param projectRoot 项目根目录。
/// @param mapDirectory 谱面文件所在目录。
/// @param path metadata 中记录的资源路径。
/// @param preferProjectRoot 是否优先按项目根目录解析。
/// @note 无法转为项目相对路径时不追加，避免包命令接收外部绝对依赖。
void appendPackageMetadataDependency(std::vector<std::string>&    dependencies,
                                     const std::filesystem::path& projectRoot,
                                     const std::filesystem::path& mapDirectory,
                                     const std::filesystem::path& path,
                                     bool preferProjectRoot)
{
    // 空 metadata 字段不代表缺失资源，无需产生诊断。
    if ( path.empty() ) return;
    // 根据源格式约定在项目根和谱面目录间选择可访问路径。
    const auto resolved = resolvePackageMetadataResourcePath(
        projectRoot, mapDirectory, path, preferProjectRoot);
    // 路径必须成功收敛到项目内部才进入依赖集合。
    appendUniquePackageDependency(
        dependencies, makePackageProjectRelativeUtf8(projectRoot, resolved));
}

/// @brief 暂存一个待批量解析的谱面音频资源引用。
/// @param references 接收引用及诊断字段的列表。
/// @param audioReference 谱面保存的音频资源 ID 或路径。
/// @param fieldLabel 引用字段的人类可读名称。
/// @note 暂存顺序必须与后续批量解析返回向量顺序一致。
void appendPackageAudioReference(std::vector<PackageAudioReference>& references,
                                 const std::string& audioReference,
                                 std::string_view   fieldLabel)
{
    // 空采样绑定表示未配置，不作为缺失依赖报告。
    if ( audioReference.empty() ) return;
    // 字段标签复制进结构，避免 string_view 指向临时文本。
    references.push_back(PackageAudioReference{
        .m_audioReference = audioReference,
        .m_fieldLabel     = std::string(fieldLabel),
    });
}

/// @brief 收集一个谱面打包时需要的资源和元素信息。
/// @param project 当前项目。
/// @param beatmapRelativePath UTF-8 项目相对谱面路径。
/// @return 谱面依赖和元素扫描结果。
/// @warning 低频文件扫描路径：读取并解析谱面，遍历全部音符和采样绑定。
/// @note 函数不修改项目或谱面文件，失败通过结果标志和诊断集合表达。
/// @details 音频引用交给 ProjectResourceService 批量解析，以复用项目对资源
/// ID、相对路径和谱面目录的兼容规则；封面路径则按源格式相对路径语义解析。
PackageBeatmapInfo collectPackageBeatmapInfo(
    const Project& project, const std::string& beatmapRelativePath)
{
    // 默认结果表示无依赖且未发生读取错误。
    PackageBeatmapInfo result;
    // 候选路径先规范化，确保与目录扫描建立的索引键一致。
    const auto normalizedBeatmapPath =
        normalizePackageRelativeUtf8(beatmapRelativePath);
    // 空候选没有可读取目标，保持默认结果交给上层候选规则处理。
    if ( normalizedBeatmapPath.empty() ) return result;

    // 仅在项目根下解析候选文件，不接受路径逃逸语义。
    const auto relativePath = Config::utf8ToPath(normalizedBeatmapPath);
    const auto mapPath =
        resolvePackageProjectPath(project.m_projectRoot, relativePath);
    // 加载器用空 map_path 表示解析失败，项目禁异常边界保持不变。
    auto beatMap = BeatMap::loadFromFile(mapPath);
    if ( beatMap.m_baseMapMetadata.map_path.empty() ) {
        // 显式标记后由 UI 阻止打包并显示缺失依赖提示。
        result.loadFailed = true;
        return result;
    }

    // 源扩展名决定 metadata 相对资源路径的优先解析根。
    const auto mapDirectory = mapPath.parent_path();
    auto       mapExtension = Config::pathToUtf8(mapPath.extension());
    mapExtension            = toLowerAscii(mapExtension);
    // MMM 按项目根保存资源路径，外部格式通常按谱面目录保存。
    const bool preferProjectRoot = packageExtensionEquals(mapExtension, ".mmm");
    const auto& meta             = beatMap.m_baseMapMetadata;

    // Flick 或 Polyline 触发 Malody 模式扩展与 Key 降级转换提示。
    result.hasStoreModeExtEligibleElements =
        !beatMap.m_noteData.flicks.empty() ||
        !beatMap.m_noteData.polylines.empty();
    // 默认主音频由统一资源服务推导，避免自行重复兼容规则。
    const auto* defaultMainAudio =
        Logic::ProjectResourceService::findDefaultBeatmapAudioResource(
            project, beatMap, relativePath);

    // 音频引用包含物件绑定和时间线自动采样两类来源。
    std::vector<PackageAudioReference> audioReferences;
    // 预留最坏情况下每个物件及采样各产生一个引用，避免反复扩容。
    audioReferences.reserve(
        beatMap.m_noteData.notes.size() + beatMap.m_noteData.holds.size() +
        beatMap.m_noteData.flicks.size() + beatMap.m_noteData.polylines.size() +
        beatMap.m_audioSamples.size());

    /// @brief 收集一个玩家物件绑定的采样资源。
    /// @param note 任意继承 Note 采样绑定接口的玩家物件。
    /// @note 未绑定采样的物件不会生成空引用。
    auto appendNoteBinding = [&](const Note& note) {
        // getSampleBinding 统一处理各类物件的可选绑定状态。
        const auto binding = note.getSampleBinding();
        if ( !binding ) return;
        appendPackageAudioReference(
            audioReferences, binding->m_audioResourceId, "Note sample");
    };
    // 四类玩家物件分别遍历，保持已有容器布局且不构造聚合副本。
    for ( const auto& note : beatMap.m_noteData.notes ) {
        appendNoteBinding(note);
    }
    for ( const auto& hold : beatMap.m_noteData.holds ) {
        appendNoteBinding(hold);
    }
    for ( const auto& flick : beatMap.m_noteData.flicks ) {
        appendNoteBinding(flick);
    }
    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        appendNoteBinding(polyline);
    }
    for ( const auto& sample : beatMap.m_audioSamples ) {
        // 自动采样字段使用独立标签，便于错误提示区别于玩家物件。
        appendPackageAudioReference(
            audioReferences, sample.m_audioResourceId, "audio_samples");
    }

    // 批量服务只需要只读视图，原字符串在本函数结束前保持稳定。
    std::vector<std::string_view> audioReferenceViews;
    audioReferenceViews.reserve(audioReferences.size());
    for ( const auto& reference : audioReferences ) {
        // 顺序不变是后续按索引关联字段标签的必要条件。
        audioReferenceViews.push_back(reference.m_audioReference);
    }
    // 一次批量解析共享谱面路径上下文，并支持 ID 与路径两种历史引用。
    const auto resolvedResources =
        Logic::ProjectResourceService::resolveAudioResourceReferences(
            project,
            Config::utf8ToPath(normalizedBeatmapPath),
            audioReferenceViews);
    // 只有真正被本谱面引用的非 OGG Main 资源才需要原点对齐选项。
    if ( defaultMainAudio && defaultMainAudio->m_type == AudioTrackType::Main &&
         !packageExtensionEquals(
             Config::pathToUtf8(
                 Config::utf8ToPath(defaultMainAudio->m_path).extension()),
             ".ogg") ) {
        // 在解析结果中查找可避免仅凭项目默认资源误判未引用音频。
        result.hasNonOggMainAudio =
            std::find(resolvedResources.begin(),
                      resolvedResources.end(),
                      defaultMainAudio) != resolvedResources.end();
    }
    // 解析结果与输入引用一一对应，逐项分类为依赖或诊断。
    for ( std::size_t index = 0; index < audioReferences.size(); ++index ) {
        const auto* resource = resolvedResources[index];
        if ( resource && !resource->m_path.empty() ) {
            // 成功解析后使用资源登记路径作为包内唯一依赖键。
            appendUniquePackageDependency(
                result.dependencyRelativePaths,
                normalizePackageRelativeUtf8(resource->m_path));
            // 已解决引用无需进入缺失列表。
            continue;
        }

        // 保留原始引用和字段来源，帮助用户修正谱面数据。
        const auto& reference = audioReferences[index];
        appendUniquePackageDependency(
            result.unresolvedAudioReferences,
            reference.m_fieldLabel + ": " + reference.m_audioReference);
    }

    // 主封面和通用封面均可能是实际包所需的图片资源。
    appendPackageMetadataDependency(result.dependencyRelativePaths,
                                    project.m_projectRoot,
                                    mapDirectory,
                                    meta.main_cover_path,
                                    preferProjectRoot);
    appendPackageMetadataDependency(result.dependencyRelativePaths,
                                    project.m_projectRoot,
                                    mapDirectory,
                                    meta.cover_path,
                                    preferProjectRoot);

    // 谱面文件自身已作为候选，不应再作为自己的依赖被锁定。
    result.dependencyRelativePaths.erase(
        std::remove(result.dependencyRelativePaths.begin(),
                    result.dependencyRelativePaths.end(),
                    normalizedBeatmapPath),
        result.dependencyRelativePaths.end());
    // 返回数据不保留 Project 或 BeatMap 内部引用。
    return result;
}
}  // namespace

/// @brief 打开谱面打包流程动作，拥有打包候选、格式选择和元数据补充状态。
/// @details 将一次打包拆为格式选择、候选依赖确认、兼容性确认、元数据补充、
/// 输出路径选择和覆盖确认等阶段。所有持久化操作最终通过逻辑命令分发。
/// @warning 实例状态仅在 UI 线程访问，延迟窗口由 renderDeferred 统一推进。
/// @note 逻辑命令按值接管最终请求，动作不跟踪后台打包完成状态。
class PackBeatmapAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在已有项目时允许打包。
    /// @param context 当前主菜单上下文，本动作仅查询编辑器项目状态。
    /// @return 当前存在项目时返回 true。
    /// @warning UI 热路径：每帧执行，不访问文件系统。
    bool isEnabled(const MainMenuContext& context) const override;

    /// @brief 打开打包格式选择流程。
    /// @param context 当前主菜单上下文。
    /// @param activation 菜单激活信息，本动作不区分触发来源。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override;

    /// @brief 渲染打包 action 拥有的延迟窗口和文件选择器。
    /// @param context 提供 DPI 和状态消息接收器的单帧上下文。
    /// @warning UI 热路径：每帧只检查弹窗状态；扫描文件仅由用户触发流程执行。
    void renderDeferred(MainMenuContext& context) override;

private:
    /// @brief 在状态栏显示打包流程消息。
    /// @param message 状态消息文本。
    /// @note 仅 renderDeferred 调用链内保证状态接收器有效。
    void showStatusMessage(std::string message);

    /// @brief 按当前打包目标格式规范化输出包路径。
    /// @param path 文件选择器返回的输出路径。
    /// @return 补齐目标打包扩展名后的输出路径。
    /// @note 总是替换而非仅追加扩展名，防止用户输入形成双扩展名。
    std::string applyPackSelectedFormatToPath(const std::string& path) const;

    /// @brief 请求打包当前已选择的项目文件。
    /// @param path 输出包路径。
    /// @note 消费 pendingRelativePaths 和 pendingMetadataOverrides
    /// 后清空临时状态。
    void requestPackBeatmapTo(std::string path);

    /// @brief 请求把当前谱面导出为独立 RM/IMD 资源包。
    /// @param path 输出 zip 路径。
    /// @note 该流程不使用常规候选列表，只分发当前谱面资源包命令。
    void requestExportImdPackageTo(std::string path);

    /// @brief 渲染打包目标格式选择弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：弹窗打开时每帧绘制固定数量按钮。
    void renderPackageFormatPickerPopup(float dpiScale);

    /// @brief 渲染打包文件复选列表窗口。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：窗口可见时遍历候选缓存，不重新扫描磁盘。
    /// @note 被已选谱面引用的媒体资源自动勾选并锁定，不能被单独取消。
    void renderPackageFileSelectionWindow(float dpiScale);

    /// @brief 继续执行打包元数据补充或输出路径选择流程。
    /// @note 仅需要人工补齐目标字段时插入元数据阶段。
    void continuePackageOutputFlow();

    /// @brief 渲染 MCZ Key 模式自动转换兼容性警告。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：仅弹窗打开时绘制静态说明和两个按钮。
    void renderPackageCompatibilityWarningPopup(float dpiScale);

    /// @brief 为选中的谱面准备打包转换前的元数据补充项。
    /// @param selectedRelativePaths 当前已选的项目相对路径列表。
    /// @return 需要展示补充窗口时返回 true。
    /// @warning 用户确认后的低频路径：会读取需要转换的谱面文件。
    bool preparePackageBeatmapMetadataEdits(
        const std::vector<std::string>& selectedRelativePaths);

    /// @brief 从补充窗口缓存收集打包元数据覆盖项。
    /// @return 元数据覆盖项列表。
    /// @note 同步把输入缓存写回 edit.baseMeta，保持后续展示状态一致。
    std::vector<Logic::PackageBeatmapMetadataOverride>
    collectPackageMetadataOverridesFromEdits();

    /// @brief 渲染打包前补充目标谱面元数据的窗口。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：窗口可见时遍历待编辑谱面和固定元数据字段。
    void renderPackageBeatmapMetadataWindow(float dpiScale);

    /// @brief 按当前目标打包格式重建候选文件列表。
    /// @warning 用户选择格式后的低频路径：递归扫描项目并解析每张谱面。
    void rebuildPackageCandidateFiles();

    /// @brief 设置候选文件选中状态，并同步谱面绑定资源。
    /// @param index 候选文件索引。
    /// @param selected 是否选中。
    /// @pre index 应来自当前 candidateFiles 的可见行。
    /// @note 越界或尝试取消锁定依赖时保持现有状态。
    void setPackageCandidateSelected(std::size_t index, bool selected);

    /// @brief 根据当前已选中谱面重新计算依赖资源锁定状态。
    /// @note 资源的选中状态由引用计数推导，谱面候选始终允许独立切换。
    void syncPackageDependencySelection();

    /// @brief 判断当前选中谱面是否存在未能绑定的依赖资源。
    /// @return 存在缺失依赖时返回 true。
    /// @note 未选中谱面的诊断不会阻塞当前打包。
    bool hasSelectedPackageMissingDependencies() const;

    /// @brief 判断当前 MCZ 候选列表是否包含可写入上架 mode_ext 的谱面。
    /// @return 存在 Flick/折线谱面且目标为 MCZ 时返回 true。
    bool hasPackageStoreModeExtCandidates() const;

    /// @brief 判断当前选中的 MCZ 谱面是否需要显示上架 mode_ext 选项。
    /// @return 存在 Flick/折线谱面且目标为 MCZ 时返回 true。
    bool hasSelectedPackageStoreModeExtCandidates() const;

    /// @brief 判断当前选中的 MCZ 谱面是否引用非 OGG 默认 Main 音频。
    /// @return 至少一个已选谱面需要原点对齐时返回 true。
    bool hasSelectedPackageNonOggMainAudio() const;

    /// @brief 收集当前已勾选的项目相对文件路径。
    /// @return 已勾选的项目相对文件路径列表。
    /// @note 保持候选排序，用于生成稳定的命令输入顺序。
    std::vector<std::string> collectSelectedPackageRelativePaths() const;

    /// @brief 生成当前打包目标格式的默认输出文件名。
    /// @return 默认输出文件名。
    /// @note 使用项目根目录名作为基名并清理路径分隔符。
    std::string makePackageDefaultFileName() const;

    /// @brief 打开谱面打包流程。
    /// @note 清理上轮瞬态状态，但保留用户最近选择的 Malody 模式。
    void openPackFilePicker();

    /// @brief 打开打包输出路径选择器。
    /// @note 根据设置选择原生 NFD 或统一 ImGuiFileDialog 实现。
    /// @pre pendingRelativePaths 已保存本次确认的候选路径。
    /// @warning 用户触发的低频路径：原生选择器可能阻塞。
    void openPackageOutputFilePicker();

    /// @brief 打开 RM/IMD 资源包输出路径选择器。
    /// @warning 用户触发的低频路径：原生选择器可能阻塞。
    /// @note 不读取常规候选状态，导出对象由逻辑层从当前会话取得。
    void openImdPackageOutputFilePicker();

    /// @brief 渲染统一打包输出文件选择器。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：仅在统一文件选择器打开时绘制。
    void renderPackageOutputFileDialog(float dpiScale);

    /// @brief 渲染统一 RM/IMD 资源包输出文件选择器。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：仅在统一文件选择器打开时绘制。
    void renderImdPackageOutputFileDialog(float dpiScale);

    /// @brief 渲染打包输出覆盖确认弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    /// @warning UI 热路径：仅待覆盖路径存在且弹窗打开时绘制。
    void renderPackageOverwriteWarningPopup(float dpiScale);

    /// @brief 当前帧状态消息接收接口，非拥有且仅在延迟渲染调用栈内有效。
    /// @warning renderDeferred 返回前必须复位为空，禁止跨帧保留。
    IStatusMessageSink* m_statusMessageSink = nullptr;
    /// @brief 打包流程的格式、候选文件和临时弹窗状态。
    /// @note 该值对象集中维护跨多个延迟窗口延续的状态机。
    /// @warning 其候选缓存仅在用户选择格式时重建，渲染时不得扫描文件系统。
    PackageDialogState m_package;
    /// @brief 是否在下一帧打开打包输出覆盖确认弹窗。
    /// @note OpenPopup 后立即复位，实际可见性由 ImGui 弹窗栈维护。
    bool m_showPackageOverwriteWarning = false;
    /// @brief 待确认覆盖的打包输出路径。
    /// @note 确认或取消后必须清空，避免误用于下一次导出。
    /// @warning 只用于显示和命令分发，本动作不直接写入该文件。
    std::string m_pendingPackageOverwritePath;
    /// @brief 待覆盖目标是否来自独立 RM/IMD 资源包流程。
    /// @note false 表示常规 MCZ、OSZ 或 MPK 打包命令。
    /// @warning 必须与待确认路径同时设置并在任一结束分支复位。
    bool m_pendingOverwriteIsImdPackage{ false };
};

/// @brief 仅在已有项目时允许打包。
/// @param context 当前主菜单上下文。
/// @return 编辑器当前项目非空时返回 true。
bool PackBeatmapAction::isEnabled(const MainMenuContext& context) const
{
    // 启用条件不依赖上下文内的窗口或焦点状态。
    (void)context;
    return Logic::EditorEngine::instance().getCurrentProject() != nullptr;
}

/// @brief 打开打包格式选择流程。
/// @param context 当前主菜单上下文。
/// @param activation 菜单激活来源。
void PackBeatmapAction::execute(MainMenuContext&              context,
                                const MainMenuItemActivation& activation)
{
    // 打包流程自行从编辑器取得当前项目，无需保存激活参数。
    (void)context;
    (void)activation;
    openPackFilePicker();
}

/// @brief 渲染打包 action 拥有的延迟窗口和文件选择器。
/// @param context 当前帧 DPI 与状态消息接口。
/// @warning UI 热路径：每帧只检查弹窗状态；扫描文件仅由用户触发流程执行。
/// @note 每个子窗口自行短路，固定调用顺序不表示它们会同时可见。
void PackBeatmapAction::renderDeferred(MainMenuContext& context)
{
    // 状态接收器只借用于本次嵌套渲染调用，防止保存悬空上下文地址。
    m_statusMessageSink = &context.statusMessageSink;
    // 各阶段自行判断打开状态，固定顺序保证新触发阶段在后续帧接管。
    renderPackageFormatPickerPopup(context.dpiScale);
    renderPackageFileSelectionWindow(context.dpiScale);
    renderPackageCompatibilityWarningPopup(context.dpiScale);
    renderPackageBeatmapMetadataWindow(context.dpiScale);
    renderPackageOutputFileDialog(context.dpiScale);
    renderImdPackageOutputFileDialog(context.dpiScale);
    renderPackageOverwriteWarningPopup(context.dpiScale);
    // 离开单帧调用前主动清空非拥有观察指针。
    m_statusMessageSink = nullptr;
}

/// @brief 在状态栏显示打包流程消息。
/// @param message 状态消息文本。
/// @note 固定显示三秒，与其他主菜单低频操作反馈保持一致。
void PackBeatmapAction::showStatusMessage(std::string message)
{
    // 非延迟渲染调用链可能没有接收器，此时安全忽略界面消息。
    if ( !m_statusMessageSink ) return;
    m_statusMessageSink->showStatusMessage(std::move(message), 3.0f);
}

/// @brief 按当前打包目标格式规范化输出包路径。
/// @param path 文件选择器返回的输出路径。
/// @return 补齐目标打包扩展名后的输出路径。
/// @note 空路径原样返回，便于取消选择器时短路。
std::string PackBeatmapAction::applyPackSelectedFormatToPath(
    const std::string& path) const
{
    if ( path.empty() ) return path;

    // 通过 filesystem 替换扩展名，正确处理用户输入的已有扩展名。
    std::filesystem::path outputPath = Config::utf8ToPath(path);
    outputPath.replace_extension(
        getPackageExtension(m_package.selectedFileType));
    return Config::pathToUtf8(outputPath);
}

/// @brief 请求打包当前已选择的项目文件。
/// @param path 输出包路径。
/// @note 所有格式特定选项都在命令构造时按目标类型重新约束。
/// @pre pendingRelativePaths 应由选择窗口确认步骤冻结。
/// @warning 分发后会消费待处理路径和元数据覆盖状态。
void PackBeatmapAction::requestPackBeatmapTo(std::string path)
{
    // 文件选择器可能允许用户输入其他扩展名，分发前统一修正。
    path = applyPackSelectedFormatToPath(path);
    if ( path.empty() || m_package.pendingRelativePaths.empty() ) {
        // 路径或候选为空都不能形成有意义的包命令。
        showStatusMessage("没有可打包的已选文件");
        return;
    }

    // 命令按值携带路径、候选和覆盖项，UI 状态可在分发后立即清理。
    MenuUtil::dispatchCommand(Logic::CmdPackBeatmap{
        .exportPath                   = path,
        .selectedProjectRelativePaths = m_package.pendingRelativePaths,
        .saveConvertedBeatmapsToProject =
            // 仅目标支持、确有转换源且用户勾选时写回转换谱面。
        shouldShowConvertedBeatmapSaveOption(m_package.selectedFileType) &&
        hasSelectedPackageBeatmapSourceRequiringConversion(
            m_package.selectedFileType, m_package.candidateFiles) &&
        m_package.saveConvertedBeatmapsToProject,
        .includeLegacyImdBeatmapsInPackage =
            // 旧 IMD 副本只对 MCZ 有效，切换格式不会泄漏旧选项。
        shouldShowLegacyImdPackageOption(m_package.selectedFileType) &&
        m_package.includeLegacyImdBeatmaps,
        .addStoreModeExtForMalodyExport =
            // mode_ext 只属于 Slide 输出且至少一张已选谱面包含扩展物件。
        m_package.selectedMalodyMode == MalodyMode::Slide &&
        hasSelectedPackageStoreModeExtCandidates() &&
        Config::AppConfig::instance()
            .getEditorSettings()
            .autoAddStoreModeExtForMalodyExport,
        .stripMainAudioVolumeFromMalodyExport =
            // 主音频音量字段清理仅适用于 Malody 导出语义。
        m_package.selectedFileType == PackageFileType::Mcz &&
        m_package.stripMainAudioVolumeFromMalodyExport,
        .alignNonOggMainAudioToOrigin =
            // 只有已选谱面真实引用非 OGG 主音频时才传递对齐请求。
        m_package.selectedFileType == PackageFileType::Mcz &&
        hasSelectedPackageNonOggMainAudio() &&
        m_package.alignNonOggMainAudioToOrigin,
        .malodyExportMode =
            // 非 MCZ 命令用空值明确不携带 Malody 模式。
        m_package.selectedFileType == PackageFileType::Mcz
            ? std::optional<MalodyMode>(m_package.selectedMalodyMode)
            : std::nullopt,
        // 覆盖项只作用于本次转换，不回写源谱面元数据。
        .metadataOverrides = m_package.pendingMetadataOverrides,
    });
    // 命令已获得值副本，清空临时列表避免重复点击再次分发同一请求。
    m_package.pendingRelativePaths.clear();
    m_package.pendingMetadataOverrides.clear();
}

/// @brief 请求把当前谱面导出为独立 RM/IMD 资源包。
/// @param path 输出 zip 路径。
/// @note 目标扩展名统一替换为 .zip，保持资源包协议稳定。
/// @warning 该函数只分发命令，不在 UI 线程执行归档写入。
void PackBeatmapAction::requestExportImdPackageTo(std::string path)
{
    // 取消或空路径不产生逻辑命令。
    if ( path.empty() ) return;
    // 即使选择器返回其他后缀，也强制输出 ZIP 容器。
    auto outputPath = Config::utf8ToPath(path);
    outputPath.replace_extension(".zip");
    // 实际资源收集与写盘由逻辑层完成，UI 只提交目标路径。
    MenuUtil::dispatchCommand(Logic::CmdExportImdPackage{
        .path = Config::pathToUtf8(outputPath),
    });
}

/// @brief 渲染打包目标格式选择弹窗。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 热路径：弹窗打开时绘制四个格式入口与取消按钮。
/// @note 选择结果在弹窗结束后处理，避免绘制栈内直接打开下一模态窗口。
/// @details 常规包格式进入候选扫描，RM/IMD ZIP 入口直接进入独立保存流程；
/// 取消则仅关闭窗口并保留最近一次格式和 Malody 模式偏好。
void PackBeatmapAction::renderPackageFormatPickerPopup(float dpiScale)
{
    // 固定 ### ID 保持中文标题与 ImGui 弹窗身份解耦。
    constexpr const char* popupId = "选择打包格式###PackageFormatPickerWindow";
    if ( m_package.showFormatPicker ) {
        // 请求标志消费一次，实际打开状态单独记录。
        ::MMM::UI::FeedbackOpenPopup(popupId);
        m_package.showFormatPicker = false;
        m_package.formatPickerOpen = true;
    }
    // 关闭后不再调用 BeginPopup，减少无效 UI 工作。
    if ( !m_package.formatPickerOpen ) return;

    // 延迟保存用户选择，确保所有 ImGui Begin/End 成对结束。
    bool            hasSelection            = false;
    bool            requestImdPackagePicker = false;
    bool            closeWindow             = false;
    PackageFileType selectedType            = m_package.selectedFileType;
    {
        // 样式作用域在离开局部块时恢复，即使弹窗未成功 Begin。
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(
                 popupId,
                 &m_package.formatPickerOpen,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking,
                 ImVec2(380.0f * dpiScale, 0.0f)) ) {
            ImGui::TextUnformatted("选择目标打包格式：");
            // 分隔标题和大尺寸格式按钮，提高模态流程辨识度。
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 固定逻辑宽度随 DPI 缩放，四个入口保持一致。
            const ImVec2 buttonSize(320.0f * dpiScale, 0.0f);
            if ( drawCenteredButton(
                     getPackageTypeDisplayName(PackageFileType::Mcz).c_str(),
                     buttonSize) ) {
                selectedType = PackageFileType::Mcz;
                // 仅记录结果，弹窗尾部统一关闭。
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
            if ( drawCenteredButton("RM/IMD 资源包 (.zip)", buttonSize) ) {
                // 独立资源包跳过候选文件选择流程。
                requestImdPackagePicker = true;
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if ( drawCenteredButton(TR("ui.common.cancel").data(),
                                    ImVec2(120.0f * dpiScale, 0.0f)) ) {
                // 取消不改变最近选择的目标格式。
                closeWindow = true;
            }

            if ( hasSelection || requestImdPackagePicker ) {
                // 任一有效入口都结束当前格式弹窗。
                closeWindow = true;
            }

            if ( closeWindow ) {
                m_package.formatPickerOpen = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // 在弹窗样式和 Begin/End 作用域外推进常规打包状态机。
    if ( hasSelection ) {
        m_package.selectedFileType = selectedType;
        if ( !shouldShowConvertedBeatmapSaveOption(selectedType) ) {
            // 切到不支持的格式时清除隐藏选项，防止旧状态进入命令。
            m_package.saveConvertedBeatmapsToProject = false;
        }
        if ( !shouldShowLegacyImdPackageOption(selectedType) ) {
            m_package.includeLegacyImdBeatmaps = false;
        }
        if ( selectedType != PackageFileType::Mcz ) {
            // Malody 专属兼容选项不得泄漏到 OSZ 或 MPK。
            m_package.stripMainAudioVolumeFromMalodyExport = false;
        }
        // 格式决定扩展名白名单，因此必须在进入文件窗口前重新扫描。
        rebuildPackageCandidateFiles();
        m_package.showFileSelectionWindow = true;
        m_package.openFileSelectionWindow = true;
    }
    if ( requestImdPackagePicker ) {
        // 独立 RM/IMD 流程直接进入输出选择器。
        openImdPackageOutputFilePicker();
    }
}

/// @brief 渲染打包文件复选列表窗口。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 热路径约束如下。
/// 热路径：打包选择弹窗可见时每帧执行；只读取候选缓存，不访问文件系统。
/// @details 顶部区域维护格式兼容选项，中部表格展示候选与依赖锁定，底部
/// 仅在至少选择一项且无缺失依赖时允许进入后续打包阶段。
void PackBeatmapAction::renderPackageFileSelectionWindow(float dpiScale)
{
    // 固定内部 ID 使窗口标题本地化后仍保留同一模态状态。
    constexpr const char* popupId = "选择打包文件###PackageFileSelectionModal";
    if ( m_package.openFileSelectionWindow ) {
        // 打开请求只消费一次，showFileSelectionWindow 负责后续持续绘制。
        ::MMM::UI::FeedbackOpenPopup(popupId);
        m_package.openFileSelectionWindow = false;
    }

    // 候选窗口关闭时不遍历候选缓存。
    if ( !m_package.showFileSelectionWindow ) return;

    // 后续阶段请求延迟到弹窗完整结束之后执行。
    bool requestOutputPicker = false;
    bool closePopup          = false;
    {
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              &m_package.showFileSelectionWindow,
                              ImGuiWindowFlags_NoCollapse,
                              ImVec2(760.0f * dpiScale, 540.0f * dpiScale),
                              false) ) {
            // 每帧从缓存统计选择数，候选变化只发生于本窗口交互。
            const auto selectedCount = static_cast<int>(
                std::count_if(m_package.candidateFiles.begin(),
                              m_package.candidateFiles.end(),
                              [](const PackageCandidateFile& file) {
                                  return file.selected;
                              }));
            ImGui::TextWrapped(
                // 标题同时显示目标格式和当前选择规模。
                "目标格式：%s  已选择：%d / %d",
                getPackageTypeDisplayName(m_package.selectedFileType).c_str(),
                selectedCount,
                static_cast<int>(m_package.candidateFiles.size()));

            if ( m_package.selectedFileType == PackageFileType::Mcz ) {
                // Malody 模式影响物件转换和 mode_ext 选项，仅 MCZ 展示。
                ImGui::TextUnformatted("打包模式：");
                ImGui::SameLine();
                const bool selectedKey =
                    m_package.selectedMalodyMode == MalodyMode::Key;
                if ( ::MMM::UI::FeedbackRadioButton("Key 模式", selectedKey) ) {
                    // Key 模式可能在确认时触发 Flick/Polyline 兼容警告。
                    m_package.selectedMalodyMode = MalodyMode::Key;
                }
                ImGui::SameLine();
                const bool selectedSlide =
                    m_package.selectedMalodyMode == MalodyMode::Slide;
                if ( ::MMM::UI::FeedbackRadioButton("Slide 模式",
                                                    selectedSlide) ) {
                    // Slide 模式可选择自动写入上架皮肤 mode_ext。
                    m_package.selectedMalodyMode = MalodyMode::Slide;
                }
            }

            // 批量选择按钮使用固定逻辑宽度，并在空间允许时同行。
            const ImVec2 selectButtonSize(88.0f * dpiScale, 0.0f);
            if ( ::MMM::UI::FeedbackButton("全选", selectButtonSize) ) {
                // 先选择全部候选，再重新计算谱面依赖锁定计数。
                for ( auto& file : m_package.candidateFiles ) {
                    file.selected = true;
                }
                syncPackageDependencySelection();
            }
            sameLineIfItemFits(selectButtonSize.x);
            if ( ::MMM::UI::FeedbackButton("全不选", selectButtonSize) ) {
                // 清空后同步会解除所有资源锁定和自动选择。
                for ( auto& file : m_package.candidateFiles ) {
                    file.selected = false;
                }
                syncPackageDependencySelection();
            }
            if ( shouldShowConvertedBeatmapSaveOption(
                     m_package.selectedFileType) ) {
                // 标签动态包含目标谱面扩展名，说明写回项目的实际文件类型。
                const std::string saveConvertedLabel =
                    "保存转换出的 " +
                    getPackageBeatmapExtension(m_package.selectedFileType) +
                    " 到项目中";
                sameLineIfItemFits(
                    getCheckboxDisplayWidth(saveConvertedLabel.c_str()));
                // 仅选中需要转换的源谱面时该选项才具有实际效果。
                const bool canSaveConvertedBeatmaps =
                    hasSelectedPackageBeatmapSourceRequiringConversion(
                        m_package.selectedFileType, m_package.candidateFiles);
                if ( !canSaveConvertedBeatmaps ) {
                    // 禁用同时清除旧值，命令分发不依赖隐藏 UI 状态。
                    m_package.saveConvertedBeatmapsToProject = false;
                    ImGui::BeginDisabled();
                }
                ::MMM::UI::FeedbackCheckbox(
                    saveConvertedLabel.c_str(),
                    &m_package.saveConvertedBeatmapsToProject);
                if ( !canSaveConvertedBeatmaps ) {
                    // AllowWhenDisabled 保证用户仍能查看不可用原因。
                    ImGui::EndDisabled();
                    if ( ImGui::IsItemHovered(
                             ImGuiHoveredFlags_AllowWhenDisabled) ) {
                        ImGui::SetTooltip(
                            "仅当已选谱面源需要转换为目标包谱面格式时可用。");
                    }
                }
            }
            if ( shouldShowLegacyImdPackageOption(
                     m_package.selectedFileType) ) {
                // 旧皮肤兼容 IMD 只在 MCZ 模式出现。
                constexpr const char* legacyImdLabel =
                    "同时打包兼容旧皮肤的 .imd";
                sameLineIfItemFits(getCheckboxDisplayWidth(legacyImdLabel));
                ::MMM::UI::FeedbackCheckbox(
                    legacyImdLabel, &m_package.includeLegacyImdBeatmaps);
                // 该值只决定包内是否附加兼容谱面，不修改原始文件。
            }
            if ( m_package.selectedFileType == PackageFileType::Mcz ) {
                // 以下两个选项调整 Malody 音频兼容性，不适用于其他包格式。
                const auto stripMainVolumeLabel =
                    TR_CACHE("ui.file.pack.strip_main_audio_volume");
                sameLineIfItemFits(
                    getCheckboxDisplayWidth(stripMainVolumeLabel.data()));
                ::MMM::UI::FeedbackCheckbox(
                    stripMainVolumeLabel.data(),
                    &m_package.stripMainAudioVolumeFromMalodyExport);
                if ( ImGui::IsItemHovered() ) {
                    // 悬停说明清除主音频音量字段的兼容影响。
                    ImGui::SetTooltip(
                        "%s",
                        TR("ui.file.pack.strip_main_audio_volume_tooltip")
                            .data());
                }

                const auto alignAudioLabel =
                    TR_CACHE("ui.file.pack.align_non_ogg_audio");
                // 只有选中谱面引用非 OGG 默认主音频时允许原点对齐。
                const bool canAlignNonOggAudio =
                    hasSelectedPackageNonOggMainAudio();
                sameLineIfItemFits(
                    getCheckboxDisplayWidth(alignAudioLabel.data()));
                if ( !canAlignNonOggAudio ) {
                    // 不可用时清除旧状态，避免切换谱面后错误携带。
                    m_package.alignNonOggMainAudioToOrigin = false;
                    ImGui::BeginDisabled();
                }
                if ( ::MMM::UI::FeedbackCheckbox(
                         alignAudioLabel.data(),
                         &m_package.alignNonOggMainAudioToOrigin) ) {
                    // 记住本次流程中的手动决定，候选刷新不得覆盖用户意图。
                    m_package.alignNonOggMainAudioUserOverridden = true;
                    // 独立保存值，选项短暂禁用后仍能恢复用户的明确决定。
                    m_package.alignNonOggMainAudioUserValue =
                        m_package.alignNonOggMainAudioToOrigin;
                }
                if ( !canAlignNonOggAudio ) {
                    ImGui::EndDisabled();
                }
                if ( ImGui::IsItemHovered(
                         canAlignNonOggAudio
                             ? ImGuiHoveredFlags_None
                             : ImGuiHoveredFlags_AllowWhenDisabled) ) {
                    // 根据是否可用显示操作说明或禁用原因。
                    ImGui::SetTooltip(
                        "%s",
                        TR(canAlignNonOggAudio
                               ? "ui.file.pack.align_non_ogg_audio_tooltip"
                               : "ui.file.pack.align_non_ogg_audio_disabled_"
                                 "tooltip")
                            .data());
                }
            }
            // 分别区分列表中存在能力与当前选择中存在能力。
            const bool hasAnyStoreModeExtCandidates =
                hasPackageStoreModeExtCandidates();
            const bool hasSelectedStoreModeExtCandidates =
                hasSelectedPackageStoreModeExtCandidates();
            if ( hasAnyStoreModeExtCandidates &&
                 m_package.selectedMalodyMode == MalodyMode::Slide ) {
                // 只有 Slide 模式能原样表达扩展物件并写入上架 mode_ext。
                constexpr const char* storeModeExtLabel =
                    "自动添加上架皮肤 mode_ext";
                sameLineIfItemFits(getCheckboxDisplayWidth(storeModeExtLabel));
                // 此选项是编辑器持久偏好，不属于单次 PackageDialogState。
                auto& settings =
                    Config::AppConfig::instance().getEditorSettings();
                bool addStoreModeExt =
                    settings.autoAddStoreModeExtForMalodyExport;
                if ( !hasSelectedStoreModeExtCandidates ) {
                    // 列表有候选但当前未选中时保留设置值、只禁用控件。
                    ImGui::BeginDisabled();
                }
                if ( ::MMM::UI::FeedbackCheckbox(storeModeExtLabel,
                                                 &addStoreModeExt) ) {
                    // 用户修改后立即写入配置，下一次打包沿用。
                    settings.autoAddStoreModeExtForMalodyExport =
                        addStoreModeExt;
                    Config::AppConfig::instance().save();
                }
                if ( !hasSelectedStoreModeExtCandidates ) {
                    // 禁用提示解释当前选择不会产生 mode_ext。
                    ImGui::EndDisabled();
                    if ( ImGui::IsItemHovered(
                             ImGuiHoveredFlags_AllowWhenDisabled) ) {
                        ImGui::SetTooltip(
                            "%s",
                            "当前未选中含 Flick/折线的谱面，打包时不会写入 "
                            "mode_ext。");
                    }
                } else if ( ImGui::IsItemHovered() ) {
                    // 可用提示说明会替换所有写出 MC 的 mode_ext。
                    ImGui::SetTooltip("%s",
                                      "打包 MCZ 时会替换所有写出的 .mc 的 "
                                      "mode_ext。");
                }
            }
            // 缺失依赖是硬阻塞条件，避免生成引用不完整的包。
            const bool hasMissingDependencies =
                hasSelectedPackageMissingDependencies();
            if ( hasMissingDependencies ) {
                // 红色摘要提示与行内具体缺失列表相互补充。
                ImGui::TextColored(
                    ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                    "%s",
                    "所选谱面缺少或当前格式不支持其引用的图片/音频资源。");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 为底部确认按钮预留高度，候选表填充其余空间。
            const ImGuiStyle& style = ImGui::GetStyle();
            const float       footerReserveHeight =
                ImGui::GetFrameHeightWithSpacing() +
                style.ItemSpacing.y * 4.0f + 2.0f * dpiScale;
            // 极窄窗口仍保证列表至少可见一行左右内容。
            const float listHeight = std::max(
                48.0f * dpiScale,
                ImGui::GetContentRegionAvail().y - footerReserveHeight);

            if ( ImGui::BeginChild("PackageCandidateFilesChild",
                                   ImVec2(0.0f, listHeight),
                                   true) ) {
                if ( m_package.candidateFiles.empty() ) {
                    // 空列表通常表示目标格式没有匹配的项目资源。
                    ImGui::TextUnformatted(
                        "没有找到符合当前打包格式规则的文件。");
                } else {
                    // 三列表格将选择状态、资源类别和相对路径分离展示。
                    constexpr ImGuiTableFlags tableFlags =
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                        ImGuiTableFlags_ScrollY;
                    if ( Utils::VerticalScrollbarStyleScope scrollbarStyle(
                             dpiScale);
                         ImGui::BeginTable("PackageCandidateFilesTable",
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

                        // 每行使用候选索引作为 ImGui ID，路径重复也不会冲突。
                        for ( std::size_t index = 0;
                              index < m_package.candidateFiles.size();
                              ++index ) {
                            auto& file = m_package.candidateFiles[index];
                            ImGui::PushID(static_cast<int>(index));
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            // 被已选谱面引用的非谱面资源必须随包携带。
                            const bool dependencyLocked =
                                file.requiredBySelectedBeatmaps > 0 &&
                                file.resourceType !=
                                    PackageResourceType::Beatmap;
                            bool selected = file.selected;
                            // 使用局部副本，仅在控件值变化时进入统一状态同步入口。
                            if ( dependencyLocked ) ImGui::BeginDisabled();
                            if ( ::MMM::UI::FeedbackCheckbox(
                                     "##PackageFileSelected", &selected) ) {
                                setPackageCandidateSelected(index, selected);
                            }
                            if ( dependencyLocked ) ImGui::EndDisabled();
                            if ( dependencyLocked &&
                                 ImGui::IsItemHovered(
                                     ImGuiHoveredFlags_AllowWhenDisabled) ) {
                                // 禁用控件仍提供锁定来源说明。
                                ImGui::SetTooltip(
                                    "该资源被已选中谱面引用，不能单独取消。");
                            }
                            ImGui::TableSetColumnIndex(1);
                            // 类型标签由目标格式规则在扫描阶段预先计算。
                            ImGui::TextUnformatted(file.typeLabel.c_str());
                            ImGui::TableSetColumnIndex(2);
                            // 相对路径使用通用正斜杠，便于跨平台识别包内位置。
                            ImGui::TextUnformatted(file.relativePath.c_str());
                            if ( file.selected &&
                                 file.resourceType ==
                                     PackageResourceType::Beatmap &&
                                 !file.missingDependencyRelativePaths
                                      .empty() ) {
                                // 仅已选谱面显示缺失标记，未选问题不阻塞打包。
                                ImGui::SameLine();
                                ImGui::TextColored(
                                    ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                                    "%s",
                                    "(缺少依赖)");
                                if ( ImGui::IsItemHovered() ) {
                                    // 逐行列出预扫描阶段保存的缺失引用。
                                    std::string tooltip = "未能绑定以下资源：";
                                    for (
                                        const auto& missingPath :
                                        file.missingDependencyRelativePaths ) {
                                        tooltip += "\n";
                                        tooltip += missingPath;
                                    }
                                    ImGui::SetTooltip("%s", tooltip.c_str());
                                }
                            }
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

            // 至少选择一个文件且所有已选谱面依赖完整时才能继续。
            const bool   canPack = selectedCount > 0 && !hasMissingDependencies;
            const ImVec2 footerButtonSize(120.0f * dpiScale, 0.0f);
            const float  footerButtonRowWidth =
                footerButtonSize.x * 2.0f + style.ItemSpacing.x;
            // 两个底部按钮作为整体居中，而不是分别居中。
            centerNextItem(footerButtonRowWidth);
            if ( !canPack ) ImGui::BeginDisabled();
            if ( ::MMM::UI::FeedbackButton("打包到...", footerButtonSize) ) {
                // 冻结当前选择为待处理路径，后续窗口不再依赖复选状态。
                m_package.pendingRelativePaths =
                    collectSelectedPackageRelativePaths();
                if ( m_package.selectedFileType == PackageFileType::Mcz &&
                     m_package.selectedMalodyMode == MalodyMode::Key &&
                     hasSelectedPackageStoreModeExtCandidates() ) {
                    // Key 模式会降级扩展物件，必须先获得用户确认。
                    m_package.showMalodyCompatibilityWarning = true;
                } else {
                    // 无兼容风险时直接进入元数据或文件选择阶段。
                    requestOutputPicker = true;
                }
                closePopup = true;
            }
            if ( !canPack ) ImGui::EndDisabled();
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           footerButtonSize) ) {
                // 取消丢弃冻结选择，保留候选缓存供下次重新扫描。
                closePopup = true;
                m_package.pendingRelativePaths.clear();
            }

            if ( closePopup ) {
                // 先关闭当前模态窗口，后续流程在作用域外启动。
                m_package.showFileSelectionWindow = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // ImGui 弹窗栈恢复后再进入下一阶段，避免嵌套模态冲突。
    if ( requestOutputPicker ) {
        continuePackageOutputFlow();
    }
}

/// @brief 继续执行打包元数据补充或输出路径选择流程。
/// @note pendingRelativePaths 必须已经由文件选择窗口冻结。
void PackBeatmapAction::continuePackageOutputFlow()
{
    // 仅确有待补字段时打开中间窗口，否则直接选择输出路径。
    if ( preparePackageBeatmapMetadataEdits(m_package.pendingRelativePaths) ) {
        m_package.showBeatmapMetadataWindow = true;
        return;
    }
    // 元数据无需补充时覆盖项保持为空。
    openPackageOutputFilePicker();
}

/// @brief 渲染 MCZ Key 模式自动转换兼容性警告。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 热路径：仅警告弹窗打开时绘制固定说明和操作按钮。
/// @note 继续操作不会修改源谱面，降级转换仅发生在打包写出阶段。
void PackBeatmapAction::renderPackageCompatibilityWarningPopup(float dpiScale)
{
    // 固定内部 ID 防止标题调整破坏模态窗口身份。
    constexpr const char* popupId =
        "谱面兼容性警告###PackageMalodyCompatibilityWarningModal";
    if ( m_package.showMalodyCompatibilityWarning ) {
        // 一次性请求转换为 ImGui 弹窗栈状态。
        ::MMM::UI::FeedbackOpenPopup(popupId);
        m_package.showMalodyCompatibilityWarning = false;
    }

    // 未打开时不创建样式作用域或计算布局。
    if ( !ImGui::IsPopupOpen(popupId) ) return;

    // 继续标志延迟到 EndPopup 后推进流程。
    bool continuePacking = false;
    {
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_None,
                              ImVec2(560.0f * dpiScale, 0.0f)) ) {
            ImGui::TextUnformatted("以 Key 模式打包前需要确认自动转换：");
            // 两条说明分别解释不兼容原因和实际降级规则。
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            MenuUtil::drawWrappedBulletText(
                "所选谱面包含 Flick 或 Polyline，而 Malody Key(0) "
                "模式无法直接存储这些物件。");
            MenuUtil::drawWrappedBulletText(
                "继续后会将 Flick 作为普通 Note 写出，忽略 Polyline 中的 "
                "subFlick，并将 subHold 作为普通 Hold 写出。");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            const ImVec2 buttonSize(120.0f * dpiScale, 0.0f);
            // 将确认和取消作为整体水平居中。
            const float buttonRowWidth =
                buttonSize.x * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            centerNextItem(buttonRowWidth);
            if ( ::MMM::UI::FeedbackButton("继续打包", buttonSize) ) {
                // 仅记录确认，下一阶段在弹窗作用域外启动。
                continuePacking = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           buttonSize) ) {
                // 取消时清除冻结路径和可能遗留的元数据覆盖。
                m_package.pendingRelativePaths.clear();
                m_package.pendingMetadataOverrides.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // 关闭警告后继续元数据检查或输出路径选择。
    if ( continuePacking ) {
        continuePackageOutputFlow();
    }
}

/// @brief 为选中的谱面准备打包转换前的元数据补充项。
/// @param selectedRelativePaths 当前已选的项目相对路径列表。
/// @return 需要展示补充窗口时返回 true。
/// @warning 用户确认后的低频路径：按需读取选中的谱面源。
/// @note 只为目标格式无法自动补齐字段的来源构建编辑项。
/// @details 空字段从谱面原文字段、文件名或项目元数据逐级回退，确保窗口
/// 给出可编辑的初始值；这些回退只有确认打包后才进入命令覆盖项。
bool PackBeatmapAction::preparePackageBeatmapMetadataEdits(
    const std::vector<std::string>& selectedRelativePaths)
{
    // 每次进入流程都清除上次缓存和覆盖项，防止跨选择复用。
    m_package.beatmapMetadataEdits.clear();
    m_package.pendingMetadataOverrides.clear();

    // 项目可能在多个模态窗口之间被关闭，必须重新验证。
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || project->m_projectRoot.empty() ) return false;

    // 仅遍历用户冻结的选择，不依赖当前候选 selected 标志。
    for ( const auto& relativePathUtf8 : selectedRelativePaths ) {
        // 规范化后再判断扩展名和拼接项目根目录。
        const auto relativePath =
            Config::utf8ToPath(relativePathUtf8).lexically_normal();
        const auto extension =
            toLowerAscii(Config::pathToUtf8(relativePath.extension()));
        if ( !shouldPreparePackageBeatmapMetadataEdit(
                 m_package.selectedFileType, extension) ) {
            // 可直接存储或可完整自动转换的谱面不需要人工介入。
            continue;
        }

        // 读取源谱面以预填目标格式要求的元数据字段。
        const auto sourcePath =
            (project->m_projectRoot / relativePath).lexically_normal();
        BeatMap beatMap = BeatMap::loadFromFile(sourcePath);
        // 读取失败由候选扫描阶段阻止；此处防御流程中途文件变化。
        if ( beatMap.m_baseMapMetadata.map_path.empty() ) continue;

        // 编辑项保留完整基础元数据，只暴露必要文本字段给用户。
        PackageBeatmapMetadataEdit edit;
        edit.relativePath = Config::pathToUtf8Generic(relativePath);
        edit.baseMeta     = beatMap.m_baseMapMetadata;

        if ( edit.baseMeta.title.empty() ) {
            // 标题依次回退原文标题和文件 stem，确保目标格式有可用值。
            edit.baseMeta.title = edit.baseMeta.title_unicode.empty()
                                      ? Config::pathToUtf8(relativePath.stem())
                                      : edit.baseMeta.title_unicode;
        }
        if ( edit.baseMeta.title_unicode.empty() ) {
            // 缺少原文标题时复用显示标题，避免写出空字段。
            edit.baseMeta.title_unicode = edit.baseMeta.title;
        }
        if ( edit.baseMeta.artist.empty() ) {
            // 谱面未提供艺术家时使用项目级元数据。
            edit.baseMeta.artist = project->m_metadata.m_artist;
        }
        if ( edit.baseMeta.artist_unicode.empty() ) {
            // 缺少艺术家原文时保持与显示字段一致。
            edit.baseMeta.artist_unicode = edit.baseMeta.artist;
        }
        if ( edit.baseMeta.author.empty() ) {
            // 制谱者回退项目 mapper，保持包列表可辨识。
            edit.baseMeta.author = project->m_metadata.m_mapper;
        }
        if ( edit.baseMeta.version.empty() ||
             edit.baseMeta.version == "unknown" ) {
            // 难度名优先取项目谱面条目显示名，再回退稳定默认值。
            auto entryIt =
                std::find_if(project->m_beatmaps.begin(),
                             project->m_beatmaps.end(),
                             [&](const MMM::Project::BeatmapEntry& entry) {
                                 return entry.m_filePath == edit.relativePath;
                             });
            // 相对路径使用与项目条目相同的 UTF-8 通用表示进行匹配。
            edit.baseMeta.version =
                entryIt != project->m_beatmaps.end() && !entryIt->m_name.empty()
                    ? entryIt->m_name
                    : "default";
        }

        // 固定缓存隔离 ImGui 编辑与元数据字符串对象生命周期。
        copyToPackageInputBuffer(edit.titleBuffer, edit.baseMeta.title);
        copyToPackageInputBuffer(edit.titleUnicodeBuffer,
                                 edit.baseMeta.title_unicode);
        copyToPackageInputBuffer(edit.artistBuffer, edit.baseMeta.artist);
        copyToPackageInputBuffer(edit.artistUnicodeBuffer,
                                 edit.baseMeta.artist_unicode);
        copyToPackageInputBuffer(edit.creatorBuffer, edit.baseMeta.author);
        copyToPackageInputBuffer(edit.versionBuffer, edit.baseMeta.version);
        // 移动进状态容器，后续窗口按候选顺序展示。
        m_package.beatmapMetadataEdits.push_back(std::move(edit));
    }

    // 非空表示状态机需要先进入元数据补充窗口。
    return !m_package.beatmapMetadataEdits.empty();
}

/// @brief 从补充窗口缓存收集打包元数据覆盖项。
/// @return 元数据覆盖项列表。
/// @note 覆盖项与编辑项顺序一致，并保留未在窗口展示的基础元数据字段。
std::vector<Logic::PackageBeatmapMetadataOverride>
PackBeatmapAction::collectPackageMetadataOverridesFromEdits()
{
    // 精确预留避免批量谱面确认时重复分配。
    std::vector<Logic::PackageBeatmapMetadataOverride> overrides;
    overrides.reserve(m_package.beatmapMetadataEdits.size());
    for ( auto& edit : m_package.beatmapMetadataEdits ) {
        // 从 NUL 终止输入缓存恢复用户最终确认的文本。
        edit.baseMeta.title = packageInputBufferText(edit.titleBuffer);
        edit.baseMeta.title_unicode =
            packageInputBufferText(edit.titleUnicodeBuffer);
        edit.baseMeta.artist = packageInputBufferText(edit.artistBuffer);
        edit.baseMeta.artist_unicode =
            packageInputBufferText(edit.artistUnicodeBuffer);
        edit.baseMeta.author  = packageInputBufferText(edit.creatorBuffer);
        edit.baseMeta.version = packageInputBufferText(edit.versionBuffer);

        // 相对路径作为逻辑层匹配源谱面的稳定键。
        overrides.push_back(Logic::PackageBeatmapMetadataOverride{
            .relativePath = edit.relativePath,
            .baseMeta     = edit.baseMeta,
        });
    }
    // 返回值按值交给 pending 状态，源编辑缓存可随后清空。
    return overrides;
}

/// @brief 渲染打包前补充目标谱面元数据的窗口。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 热路径：窗口可见时遍历待编辑谱面并绘制固定字段表。
/// @note 确认只生成本次打包覆盖项，不直接修改源谱面文件。
/// @details 每张谱面以相对路径作为折叠标题，并提供目标格式常用的标题、
/// 艺术家、制谱者和版本字段；滚动区域始终为底部操作按钮保留空间。
void PackBeatmapAction::renderPackageBeatmapMetadataWindow(float dpiScale)
{
    // 固定内部 ID 使标题调整不影响模态状态。
    constexpr const char* popupId =
        "补充谱面元数据###PackageBeatmapMetadataModal";
    if ( m_package.showBeatmapMetadataWindow ) {
        // 一次性打开请求转交 ImGui 弹窗栈管理。
        ::MMM::UI::FeedbackOpenPopup(popupId);
        m_package.showBeatmapMetadataWindow = false;
    }

    // 未打开时不遍历可能较大的谱面编辑列表。
    if ( !ImGui::IsPopupOpen(popupId) ) return;

    // 输出选择器必须在当前弹窗 EndPopup 后再打开。
    bool requestOutputPicker = false;
    {
        Utils::CenteredModalPopupScope popupStyle(dpiScale);
        if ( popupStyle.begin(popupId,
                              nullptr,
                              ImGuiWindowFlags_NoCollapse,
                              ImVec2(720.0f * dpiScale, 520.0f * dpiScale),
                              false) ) {
            // 提示按目标格式说明转换后的谱面扩展名和字段要求。
            const std::string prompt =
                makePackageMetadataEditPrompt(m_package.selectedFileType);
            ImGui::TextUnformatted(prompt.c_str());
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 为底部操作按钮预留固定高度，编辑区占满其余空间。
            const ImGuiStyle& style = ImGui::GetStyle();
            const float       footerReserveHeight =
                ImGui::GetFrameHeightWithSpacing() +
                style.ItemSpacing.y * 4.0f + 2.0f * dpiScale;
            // 窄窗口仍保留最小编辑高度以显示至少一组字段。
            const float editHeight = std::max(
                120.0f * dpiScale,
                ImGui::GetContentRegionAvail().y - footerReserveHeight);

            {
                Utils::VerticalScrollbarStyleScope scrollbarStyle(dpiScale);
                // 独立滚动区域避免大量谱面把底部确认按钮推出窗口。
                if ( ImGui::BeginChild("PackageBeatmapMetadataEditChild",
                                       ImVec2(0.0f, editHeight),
                                       true) ) {
                    // 索引作为 ImGui ID，两个同名相对路径也不会冲突控件。
                    for ( std::size_t index = 0;
                          index < m_package.beatmapMetadataEdits.size();
                          ++index ) {
                        auto& edit = m_package.beatmapMetadataEdits[index];
                        ImGui::PushID(static_cast<int>(index));
                        if ( ::MMM::UI::FeedbackCollapsingHeader(
                                 edit.relativePath.c_str(),
                                 ImGuiTreeNodeFlags_DefaultOpen) ) {
                            // 每张谱面使用两列表格对齐字段标签与输入框。
                            if ( ImGui::BeginTable(
                                     "PackageBeatmapMetadataFields",
                                     2,
                                     ImGuiTableFlags_SizingStretchProp |
                                         ImGuiTableFlags_NoSavedSettings) ) {
                                ImGui::TableSetupColumn(
                                    "字段",
                                    ImGuiTableColumnFlags_WidthFixed,
                                    112.0f * dpiScale);
                                ImGui::TableSetupColumn(
                                    "值", ImGuiTableColumnFlags_WidthStretch);

                                /// @brief 绘制单个元数据标签和输入缓存行。
                                /// @param label 用户可见字段名。
                                /// @param id 当前谱面作用域内的 ImGui ID。
                                /// @param buffer 固定容量可写输入缓存。
                                /// @warning UI 热路径：仅布局并绘制一个
                                /// InputText。
                                auto inputRow = [](const char* label,
                                                   const char* id,
                                                   auto&       buffer) {
                                    // 标签列固定宽度，输入列占用剩余空间。
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0);
                                    ImGui::AlignTextToFramePadding();
                                    ImGui::TextUnformatted(label);
                                    ImGui::TableSetColumnIndex(1);
                                    ImGui::SetNextItemWidth(-1.0f);
                                    ImGui::InputText(
                                        id, buffer.data(), buffer.size());
                                };

                                // 字段名称对应目标格式常见元数据语义。
                                inputRow("Title", "##Title", edit.titleBuffer);
                                inputRow("TitleOrg",
                                         "##TitleUnicode",
                                         edit.titleUnicodeBuffer);
                                inputRow(
                                    "Artist", "##Artist", edit.artistBuffer);
                                inputRow("ArtistOrg",
                                         "##ArtistUnicode",
                                         edit.artistUnicodeBuffer);
                                inputRow(
                                    "Creator", "##Creator", edit.creatorBuffer);
                                inputRow(
                                    "Version", "##Version", edit.versionBuffer);

                                ImGui::EndTable();
                            }
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // 确认和取消按钮按组居中。
            const ImVec2 buttonSize(120.0f * dpiScale, 0.0f);
            const float  buttonRowWidth =
                buttonSize.x * 2.0f + style.ItemSpacing.x;
            centerNextItem(buttonRowWidth);
            if ( ::MMM::UI::FeedbackButton("继续打包", buttonSize) ) {
                // 将输入冻结为逻辑层覆盖项后即可释放 UI 编辑缓存。
                m_package.pendingMetadataOverrides =
                    collectPackageMetadataOverridesFromEdits();
                m_package.beatmapMetadataEdits.clear();
                // 输出选择延迟到当前模态窗口关闭之后。
                requestOutputPicker = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           buttonSize) ) {
                // 取消终止整次流程并清除所有阶段性选择。
                m_package.pendingRelativePaths.clear();
                m_package.pendingMetadataOverrides.clear();
                m_package.beatmapMetadataEdits.clear();
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    // ImGui 弹窗作用域结束后再打开文件选择器。
    if ( requestOutputPicker ) {
        openPackageOutputFilePicker();
    }
}

/// @brief 按当前目标打包格式重建候选文件列表。
/// @warning 低频 UI
/// 路径：打开打包选择窗口或切换格式时执行；会扫描项目目录并读取谱面元数据。
/// @note 候选排序、路径规范化和依赖分析全部在窗口打开前一次完成。
/// @details 首轮目录扫描建立目标格式允许的文件集合，第二轮解析谱面依赖，
/// 第三轮根据打开会话推导默认选择，最后统一锁定被选谱面引用的资源。
void PackBeatmapAction::rebuildPackageCandidateFiles()
{
    // 先丢弃旧格式候选，扫描失败时不会误显示过期文件。
    m_package.candidateFiles.clear();

    // 格式选择后项目可能已关闭，扫描前重新验证根目录。
    auto* project = Logic::EditorEngine::instance().getCurrentProject();
    if ( !project || project->m_projectRoot.empty() ) return;

    // 目标格式规则决定可读取谱面源和可直接打包的资源扩展名。
    const auto& types =
        getPackageSupportedFileTypes(m_package.selectedFileType);
    const auto& projectRoot = project->m_projectRoot;

    // 根目录检查使用 error_code，保持项目禁异常约束。
    std::error_code filesystemError;
    if ( !std::filesystem::exists(projectRoot, filesystemError) ||
         filesystemError ||
         !std::filesystem::is_directory(projectRoot, filesystemError) ||
         filesystemError ) {
        showStatusMessage("扫描项目文件失败");
        // 不构造部分候选，窗口随后展示空列表。
        return;
    }

    // 权限不足的子目录跳过，其他迭代错误仍通过状态栏报告。
    constexpr auto directoryOptions =
        std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator iterator(
        projectRoot, directoryOptions, filesystemError);
    const std::filesystem::recursive_directory_iterator endIterator;
    if ( filesystemError ) {
        showStatusMessage("扫描项目文件失败");
        return;
    }

    // 目录只在此低频阶段完整遍历，UI 每帧仅使用缓存。
    while ( iterator != endIterator ) {
        const auto& entry = *iterator;
        if ( entry.is_regular_file(filesystemError) && !filesystemError ) {
            // 只处理普通文件，目录和特殊节点不进入包候选。
            const auto path = entry.path();
            const auto extension =
                toLowerAscii(Config::pathToUtf8(path.extension()));
            // 标签和枚举分别服务 UI 展示与后续依赖逻辑。
            const auto typeLabel =
                getPackageCandidateTypeLabel(types, extension);
            const auto resourceType =
                getPackageCandidateResourceType(types, extension);
            // 候选必须能转换为项目相对路径，包内不写绝对路径。
            std::error_code relativeError;
            const auto      relativePath =
                std::filesystem::relative(path, projectRoot, relativeError);
            if ( !typeLabel.empty() && resourceType &&
                 !isPackageArchiveExtension(extension) && !relativeError ) {
                // 初始不选择，稍后根据打开会话推导谱面默认状态。
                m_package.candidateFiles.push_back(PackageCandidateFile{
                    .relativePath = Config::pathToUtf8Generic(relativePath),
                    .typeLabel    = typeLabel,
                    .resourceType = *resourceType,
                    .selected     = false,
                });
            }
        }
        // 单项类型查询错误不污染迭代器 increment 的独立错误判断。
        filesystemError.clear();

        iterator.increment(filesystemError);
        if ( filesystemError ) {
            // 保留已扫描候选供诊断，但明确提示结果可能不完整。
            showStatusMessage("扫描项目文件失败");
            break;
        }
    }

    // 先按类型再按路径排序，使候选窗口分组稳定且便于查找。
    std::sort(
        m_package.candidateFiles.begin(),
        m_package.candidateFiles.end(),
        [](const PackageCandidateFile& lhs, const PackageCandidateFile& rhs) {
            if ( lhs.typeLabel != rhs.typeLabel ) {
                // 类型标签排序将谱面、音频、视频、图片形成连续组。
                return lhs.typeLabel < rhs.typeLabel;
            }
            return lhs.relativePath < rhs.relativePath;
        });

    // 路径索引供谱面依赖存在性检查使用，避免对候选反复线性扫描。
    std::unordered_map<std::string, std::size_t> candidateIndexByPath;
    candidateIndexByPath.reserve(m_package.candidateFiles.size());
    for ( std::size_t index = 0; index < m_package.candidateFiles.size();
          ++index ) {
        // 规范化为 UTF-8 通用分隔符，统一来自 filesystem 的平台差异。
        m_package.candidateFiles[index].relativePath =
            normalizePackageRelativeUtf8(
                m_package.candidateFiles[index].relativePath);
        candidateIndexByPath[m_package.candidateFiles[index].relativePath] =
            index;
    }

    // 只有谱面需要解析内部资源引用和格式兼容性。
    for ( auto& file : m_package.candidateFiles ) {
        if ( file.resourceType != PackageResourceType::Beatmap ) continue;

        // 每张谱面只读取一次，扫描结果移动到候选缓存。
        auto beatmapInfo =
            collectPackageBeatmapInfo(*project, file.relativePath);
        file.dependencyRelativePaths =
            std::move(beatmapInfo.dependencyRelativePaths);
        file.hasStoreModeExtEligibleElements =
            beatmapInfo.hasStoreModeExtEligibleElements;
        file.hasNonOggMainAudio = beatmapInfo.hasNonOggMainAudio;
        file.missingDependencyRelativePaths =
            std::move(beatmapInfo.unresolvedAudioReferences);
        if ( beatmapInfo.loadFailed ) {
            // 读取失败作为缺失依赖处理，阻止用户生成不可验证的包。
            file.missingDependencyRelativePaths.push_back(
                "谱面文件读取失败，无法检查依赖");
        }
        // 已解析路径若不在当前格式候选中，仍视为不支持或缺失。
        for ( const auto& dependencyPath : file.dependencyRelativePaths ) {
            if ( candidateIndexByPath.find(dependencyPath) ==
                 candidateIndexByPath.end() ) {
                appendUniquePackageDependency(
                    file.missingDependencyRelativePaths, dependencyPath);
            }
        }
    }

    // 当前打开且可见的谱面决定默认勾选状态。
    auto&         engine             = Logic::EditorEngine::instance();
    auto          sessionEntries     = engine.getSessionEntries();
    const int32_t activeSessionIndex = engine.getActiveSessionIndex();
    std::vector<PackageOpenBeatmapState> openBeatmaps;
    // 转为轻量状态列表，避免默认选择策略依赖编辑器内部会话类型。
    openBeatmaps.reserve(sessionEntries.size());
    for ( std::size_t index = 0; index < sessionEntries.size(); ++index ) {
        const auto& entry = sessionEntries[index];
        openBeatmaps.push_back(PackageOpenBeatmapState{
            .beatmapPathKey  = entry.beatmapPathKey,
            .isCanvasVisible = entry.isCanvasVisible,
            .isActive = static_cast<int32_t>(index) == activeSessionIndex,
            .isLogoPlaceholder = entry.isLogoPlaceholder,
        });
    }

    // 非谱面资源起初不选，由已选谱面的依赖同步自动锁定。
    for ( auto& file : m_package.candidateFiles ) {
        if ( file.resourceType != PackageResourceType::Beatmap ) {
            file.selected = false;
            continue;
        }
        // 绝对候选路径通过编辑器统一规则生成会话比较键。
        const auto candidatePath =
            projectRoot / Config::utf8ToPath(file.relativePath);
        const std::string candidatePathKey =
            engine.makeBeatmapPathKeyForPath(candidatePath);
        file.selected =
            shouldDefaultSelectPackageBeatmap(candidatePathKey, openBeatmaps);
    }

    // 默认选中谱面确定后，一次同步所有资源依赖引用计数。
    syncPackageDependencySelection();
    if ( !hasSelectedPackageBeatmapSourceRequiringConversion(
             m_package.selectedFileType, m_package.candidateFiles) ) {
        m_package.saveConvertedBeatmapsToProject = false;
        // 没有转换源时隐藏选项对应状态必须复位。
    }
}

/// @brief 设置候选文件选中状态，并同步谱面绑定资源。
/// @param index 候选文件索引。
/// @param selected 是否选中。
/// @warning UI 热路径：打包选择弹窗可见时由用户操作触发；只更新候选列表缓存。
void PackBeatmapAction::setPackageCandidateSelected(std::size_t index,
                                                    bool        selected)
{
    // 防御过期 UI 索引，候选重建后旧事件不得访问越界元素。
    if ( index >= m_package.candidateFiles.size() ) return;

    // 被谱面依赖锁定的资源不能单独取消，但谱面自身始终可切换。
    auto& file = m_package.candidateFiles[index];
    if ( !selected && file.requiredBySelectedBeatmaps > 0 &&
         file.resourceType != PackageResourceType::Beatmap ) {
        // 保持选中状态和引用计数不变。
        return;
    }

    // 更新用户意图后立即重算全部派生资源选择。
    file.selected = selected;
    syncPackageDependencySelection();
    if ( !hasSelectedPackageBeatmapSourceRequiringConversion(
             m_package.selectedFileType, m_package.candidateFiles) ) {
        // 最后一张转换源取消后清除已失效的写回选项。
        m_package.saveConvertedBeatmapsToProject = false;
    }
}

/// @brief 根据当前已选中谱面重新计算依赖资源锁定状态。
/// @warning UI 热路径：打包选择弹窗可见时由用户操作触发；按候选数量线性更新。
/// @details 每次从已选谱面完整重建引用计数，避免多次全选、取消和切换谱面
/// 后增量计数漂移。解除最后一个引用时会取消此前自动选中的媒体资源。
void PackBeatmapAction::syncPackageDependencySelection()
{
    // 记录上一轮锁定状态，用于依赖解除时自动取消资源。
    std::vector<bool> wasLocked;
    wasLocked.reserve(m_package.candidateFiles.size());
    for ( auto& file : m_package.candidateFiles ) {
        // 所有引用计数从当前谱面选择重新推导，不做增量修补。
        wasLocked.push_back(file.requiredBySelectedBeatmaps > 0);
        file.requiredBySelectedBeatmaps = 0;
    }

    // 路径到候选索引的哈希表降低多谱面依赖同步复杂度。
    std::unordered_map<std::string, std::size_t> candidateIndexByPath;
    candidateIndexByPath.reserve(m_package.candidateFiles.size());
    for ( std::size_t index = 0; index < m_package.candidateFiles.size();
          ++index ) {
        candidateIndexByPath[m_package.candidateFiles[index].relativePath] =
            index;
    }

    // 只有已选谱面的依赖会产生锁定引用计数。
    for ( const auto& file : m_package.candidateFiles ) {
        if ( !file.selected ||
             file.resourceType != PackageResourceType::Beatmap ) {
            continue;
        }

        for ( const auto& dependencyPath : file.dependencyRelativePaths ) {
            // 扫描阶段已记录缺失项，这里只处理存在的候选资源。
            const auto dependencyIt = candidateIndexByPath.find(dependencyPath);
            if ( dependencyIt == candidateIndexByPath.end() ) continue;
            auto& dependencyFile =
                m_package.candidateFiles[dependencyIt->second];
            // 计数而非布尔值便于多个已选谱面共享同一资源。
            dependencyFile.requiredBySelectedBeatmaps++;
        }
    }

    // 最终选择状态由新引用计数和上一轮锁定状态共同决定。
    for ( std::size_t index = 0; index < m_package.candidateFiles.size();
          ++index ) {
        auto& file = m_package.candidateFiles[index];
        if ( file.requiredBySelectedBeatmaps > 0 ) {
            // 依赖资源强制随至少一张已选谱面进入包。
            file.selected = true;
        } else if ( index < wasLocked.size() && wasLocked[index] &&
                    file.resourceType != PackageResourceType::Beatmap ) {
            // 曾仅因依赖自动选中的资源在最后引用解除时自动取消。
            file.selected = false;
        }
    }

    // 初次出现非 OGG 主音轨时默认开启对齐；用户手动修改后只处理失效清空。
    m_package.alignNonOggMainAudioToOrigin =
        resolveNonOggAudioAlignmentSelection(
            hasSelectedPackageNonOggMainAudio(),
            m_package.alignNonOggMainAudioUserOverridden,
            m_package.alignNonOggMainAudioUserValue);
}

/// @brief 判断当前选中谱面是否存在未能绑定的依赖资源。
/// @return 存在缺失依赖时返回 true。
/// @warning UI 热路径：打包选择弹窗可见时每帧查询；只遍历候选缓存。
bool PackBeatmapAction::hasSelectedPackageMissingDependencies() const
{
    // 未选谱面的缺失资源不影响当前打包请求。
    return std::any_of(m_package.candidateFiles.begin(),
                       m_package.candidateFiles.end(),
                       [](const PackageCandidateFile& file) {
                           return file.selected &&
                                  file.resourceType ==
                                      PackageResourceType::Beatmap &&
                                  !file.missingDependencyRelativePaths.empty();
                       });
}

/// @brief 判断当前 MCZ 候选列表是否存在可写入上架 mode_ext 的谱面。
/// @return 存在 Flick/折线谱面且目标为 MCZ 时返回 true。
/// @warning UI 热路径：打包选择弹窗可见时每帧查询；只遍历候选缓存。
/// @note 此查询忽略 selected，用于决定控件是否需要出现在窗口中。
bool PackBeatmapAction::hasPackageStoreModeExtCandidates() const
{
    // mode_ext 是 Malody 专属字段，其他格式直接短路。
    if ( m_package.selectedFileType != PackageFileType::Mcz ) {
        return false;
    }
    // 此查询忽略选择状态，用于决定是否显示选项本身。
    return std::any_of(m_package.candidateFiles.begin(),
                       m_package.candidateFiles.end(),
                       [](const PackageCandidateFile& file) {
                           return file.resourceType ==
                                      PackageResourceType::Beatmap &&
                                  file.hasStoreModeExtEligibleElements;
                       });
}

/// @brief 判断当前选中的 MCZ 谱面是否需要显示上架 mode_ext 选项。
/// @return 存在 Flick/折线谱面且目标为 MCZ 时返回 true。
/// @warning UI 热路径：打包选择弹窗可见时每帧查询；只遍历候选缓存。
/// @note 此查询包含 selected，用于决定已显示控件是否启用。
bool PackBeatmapAction::hasSelectedPackageStoreModeExtCandidates() const
{
    // 非 MCZ 目标不具备上架模式扩展语义。
    if ( m_package.selectedFileType != PackageFileType::Mcz ) {
        return false;
    }
    // 仅当前选择用于决定控件启用状态和最终命令字段。
    return std::any_of(m_package.candidateFiles.begin(),
                       m_package.candidateFiles.end(),
                       [](const PackageCandidateFile& file) {
                           return file.selected &&
                                  file.resourceType ==
                                      PackageResourceType::Beatmap &&
                                  file.hasStoreModeExtEligibleElements;
                       });
}

/// @brief 判断当前选中的 MCZ 谱面是否引用非 OGG 默认 Main 音频。
/// @return 至少一个已选谱面需要原点对齐时返回 true。
/// @warning UI 热路径：打包选择弹窗可见时每帧查询；只遍历候选缓存。
/// @note 标志在谱面扫描阶段仅对实际解析到的默认 Main 资源设置。
bool PackBeatmapAction::hasSelectedPackageNonOggMainAudio() const
{
    // 音频原点对齐是 MCZ 兼容措施，其他格式无需检查。
    if ( m_package.selectedFileType != PackageFileType::Mcz ) return false;
    return std::any_of(m_package.candidateFiles.begin(),
                       m_package.candidateFiles.end(),
                       [](const PackageCandidateFile& file) {
                           return file.selected &&
                                  file.resourceType ==
                                      PackageResourceType::Beatmap &&
                                  file.hasNonOggMainAudio;
                       });
}

/// @brief 收集当前已勾选的项目相对文件路径。
/// @return 已勾选的项目相对文件路径列表。
/// @warning UI 热路径低频分支：点击确认打包时执行；只遍历候选缓存。
/// @note 返回结果包含自动锁定依赖，逻辑层无需再次推导 UI 选择状态。
std::vector<std::string>
PackBeatmapAction::collectSelectedPackageRelativePaths() const
{
    // 最坏情况全部选中，预留候选总数避免确认路径上多次扩容。
    std::vector<std::string> selectedPaths;
    selectedPaths.reserve(m_package.candidateFiles.size());
    for ( const auto& file : m_package.candidateFiles ) {
        if ( file.selected ) {
            // 路径已在候选重建阶段规范化，可直接交给逻辑命令。
            selectedPaths.push_back(file.relativePath);
        }
    }
    return selectedPaths;
}

/// @brief 生成当前打包目标格式的默认输出文件名。
/// @return 默认输出文件名。
/// @warning 用户触发的低频路径：只读取当前项目根目录名。
std::string PackBeatmapAction::makePackageDefaultFileName() const
{
    // 无项目或根目录名为空时使用通用 map 基名。
    std::string baseName = "map";
    auto*       project  = Logic::EditorEngine::instance().getCurrentProject();
    if ( project && !project->m_projectRoot.empty() ) {
        // filename 只取项目根最后一段，不把父目录带入保存文件名。
        baseName = Config::pathToUtf8(project->m_projectRoot.filename());
    }
    if ( baseName.empty() ) baseName = "map";
    // 文件名片段清理后追加当前目标格式扩展名。
    return sanitizePackageFileNamePart(baseName) +
           getPackageExtension(m_package.selectedFileType);
}

/// @brief 打开谱面打包流程。
/// @note 该入口只初始化状态；项目扫描发生在用户选定目标格式之后。
/// @warning UI 低频路径：仅修改动作状态，不打开阻塞式文件选择器。
/// @details 输出覆盖状态由文件选择器维护，本入口不删除任何既有文件。
void PackBeatmapAction::openPackFilePicker()
{
    // 清除所有依赖目标格式或上次候选选择的瞬态数据。
    m_package.candidateFiles.clear();
    m_package.pendingRelativePaths.clear();
    m_package.pendingMetadataOverrides.clear();
    m_package.beatmapMetadataEdits.clear();
    // 新流程重新采用非 OGG 安全默认值，不继承上一次窗口的手动选择。
    m_package.alignNonOggMainAudioToOrigin       = false;
    m_package.alignNonOggMainAudioUserOverridden = false;
    m_package.alignNonOggMainAudioUserValue      = false;
    // 关闭后续阶段的打开标志，防止旧弹窗在新流程中重现。
    m_package.showFileSelectionWindow        = false;
    m_package.openFileSelectionWindow        = false;
    m_package.showBeatmapMetadataWindow      = false;
    m_package.showMalodyCompatibilityWarning = false;
    // 保留用户最近选择的 Malody 打包模式，下一次打开时继续沿用。
    m_package.formatPickerOpen = false;
    // 下一帧由延迟渲染入口打开格式选择弹窗。
    m_package.showFormatPicker = true;
    if ( !shouldShowLegacyImdPackageOption(m_package.selectedFileType) ) {
        // 防御上次非 MCZ 状态残留旧格式专属选项。
        m_package.includeLegacyImdBeatmaps = false;
    }
    if ( m_package.selectedFileType != PackageFileType::Mcz ) {
        // Malody 音频兼容设置只在 MCZ 目标下保留。
        m_package.stripMainAudioVolumeFromMalodyExport = false;
    }
}

/// @brief 打开打包输出路径选择器。
/// @warning 用户触发的低频路径：原生选择器会阻塞直到用户完成交互。
/// @note 两种选择器最终都执行扩展名修正和已存在文件确认。
/// @pre 常规候选和所需元数据覆盖已冻结在 m_package 中。
void PackBeatmapAction::openPackageOutputFilePicker()
{
    // 设置对象决定选择器实现，并提供无项目时的最近目录回退。
    auto& config = Config::AppConfig::instance().getEditorSettings();
    // 默认文件名在打开选择器前按当前格式构造。
    const std::string defaultFileName = makePackageDefaultFileName();
    const std::string defaultPath     = getPackagePickerDefaultPath(config);
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        // 原生选择器打开前播放统一弹窗反馈。
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t* outPath = nullptr;
        const char*  packageFilter =
            getNativePackageOutputFilterText(m_package.selectedFileType);
        // NFD 过滤器使用不带点的目标扩展名。
        nfdu8filteritem_t filters[1] = { { "Beatmap Package", packageFilter } };
        nfdresult_t       result     = NativeFileDialog::saveFile(
            &outPath, filters, 1, defaultPath.c_str(), defaultFileName.c_str());

        if ( result == NFD_OKAY ) {
            // 用户路径再次替换扩展名，确保与当前格式一致。
            std::string filePath = applyPackSelectedFormatToPath(outPath);
            if ( std::filesystem::exists(Config::utf8ToPath(filePath)) ) {
                // 已存在目标延迟到自有覆盖确认弹窗处理。
                m_pendingPackageOverwritePath  = std::move(filePath);
                m_pendingOverwriteIsImdPackage = false;
                m_showPackageOverwriteWarning  = true;
            } else {
                // 新路径可立即分发打包命令。
                requestPackBeatmapTo(std::move(filePath));
            }
            // NFD 返回路径由库分配，成功分支必须释放。
            NFD_FreePathU8(outPath);
        }
    } else {
        // 统一选择器以非阻塞 ImGui 窗口形式在后续帧渲染。
        IGFD::FileDialogConfig fdConfig;
        fdConfig.path = defaultPath;
        // 保存操作只允许一个输出目标。
        fdConfig.countSelectionMax = 1;
        fdConfig.fileName          = defaultFileName;
        fdConfig.flags =
            ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;
        const char* packageFilter =
            getUnifiedPackageOutputFilterText(m_package.selectedFileType);
        // 记录先前状态，确保只在首次真正打开时播放反馈。
        const bool wasOpen =
            ImGuiFileDialog::Instance()->IsOpened("PackFilePicker");
        // 对话框键与 renderPackageOutputFileDialog 保持一致。
        ImGuiFileDialog::Instance()->OpenDialog("PackFilePicker",
                                                TR("ui.file.pack").data(),
                                                packageFilter,
                                                fdConfig);
        if ( !wasOpen &&
             ImGuiFileDialog::Instance()->IsOpened("PackFilePicker") ) {
            ::MMM::UI::PlayPopupOpenFeedback();
        }
    }
}

/// @brief 打开 RM/IMD 资源包输出路径选择器。
/// @warning 用户触发的低频路径：原生选择器可能阻塞。
/// @note 独立资源包固定使用 ZIP，与常规包格式选择无关。
/// @details 原生与统一选择器共用默认目录和文件名规则，确认后均检查目标
/// 是否存在，并通过同一覆盖弹窗延迟实际导出请求。
void PackBeatmapAction::openImdPackageOutputFilePicker()
{
    // 沿用通用导出命名逻辑，从当前谱面推导默认文件名。
    auto& config = Config::AppConfig::instance().getEditorSettings();
    const std::string defaultFileName =
        MenuUtil::makeExportFileNameForExtension(".zip", "map.zip");
    const std::string defaultPath = getPackagePickerDefaultPath(config);
    if ( config.filePickerStyle == Config::FilePickerStyle::Native ) {
        // 原生保存窗口是用户显式触发的允许阻塞路径。
        ::MMM::UI::PlayPopupOpenFeedback();
        nfdu8char_t*      outPath    = nullptr;
        nfdu8filteritem_t filters[1] = { { "RM/IMD 资源包", "zip" } };
        const nfdresult_t result     = NativeFileDialog::saveFile(
            &outPath, filters, 1, defaultPath.c_str(), defaultFileName.c_str());
        if ( result == NFD_OKAY ) {
            // 统一替换为 .zip，避免用户输入错误容器后缀。
            auto outputPath = Config::utf8ToPath(outPath);
            outputPath.replace_extension(".zip");
            std::string filePath = Config::pathToUtf8(outputPath);
            // 覆盖检测使用 error_code，避免文件系统异常越过 UI 边界。
            std::error_code filesystemError;
            if ( std::filesystem::exists(outputPath, filesystemError) &&
                 !filesystemError ) {
                m_pendingPackageOverwritePath = std::move(filePath);
                // 标记确认后应分发独立资源包命令而非常规打包命令。
                m_pendingOverwriteIsImdPackage = true;
                m_showPackageOverwriteWarning  = true;
            } else {
                // 不存在或查询出错时按新输出目标提交逻辑层处理。
                requestExportImdPackageTo(std::move(filePath));
            }
            NFD_FreePathU8(outPath);
        }
        // 原生选择器已完成同步交互，无需配置统一选择器。
        return;
    }

    // 统一选择器由延迟渲染入口逐帧推进。
    IGFD::FileDialogConfig fdConfig;
    fdConfig.path              = defaultPath;
    fdConfig.countSelectionMax = 1;
    fdConfig.fileName          = defaultFileName;
    fdConfig.flags =
        ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_HideColumnType;
    // 仅首次打开播放声音，避免每帧重复反馈。
    const bool wasOpen =
        ImGuiFileDialog::Instance()->IsOpened("ImdPackageFilePicker");
    ImGuiFileDialog::Instance()->OpenDialog(
        "ImdPackageFilePicker", "导出 RM/IMD 资源包", ".zip", fdConfig);
    if ( !wasOpen &&
         ImGuiFileDialog::Instance()->IsOpened("ImdPackageFilePicker") ) {
        ::MMM::UI::PlayPopupOpenFeedback();
    }
}

/// @brief 渲染统一打包输出文件选择器。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 热路径：仅在统一文件选择器打开时绘制。
/// @note 确认后更新最近目录，并根据目标存在性进入覆盖确认或直接分发。
/// @warning 文件存在性只在用户确认分支查询，不进入空闲帧热路径。
/// @pre PackFilePicker 已由 openPackageOutputFilePicker 打开。
void PackBeatmapAction::renderPackageOutputFileDialog(float dpiScale)
{
    // 样式作用域统一缩放和居中 ImGuiFileDialog。
    Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
    if ( ImGuiFileDialog::Instance()->IsOpened("PackFilePicker") ) {
        // 在 Display 前设置下一窗口位置，避免首帧跳动。
        Utils::prepareCenteredModalWindow({ 600, 400 });
    }
    if ( ImGuiFileDialog::Instance()->Display(
             "PackFilePicker",
             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings,
             { 600, 400 }) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            // 用户确认路径后强制应用当前包扩展名。
            std::string filePath =
                ImGuiFileDialog::Instance()->GetFilePathName();
            filePath = applyPackSelectedFormatToPath(filePath);

            // 保存统一选择器当前目录供后续所有文件操作复用。
            auto& engine = Logic::EditorEngine::instance();
            auto  config = engine.getEditorConfig();
            config.settings.lastFilePickerPath =
                ImGuiFileDialog::Instance()->GetCurrentPath();
            engine.setEditorConfig(config);

            // 已存在文件需显式确认，防止无提示覆盖。
            if ( std::filesystem::exists(Config::utf8ToPath(filePath)) ) {
                m_pendingPackageOverwritePath  = std::move(filePath);
                m_pendingOverwriteIsImdPackage = false;
                m_showPackageOverwriteWarning  = true;
            } else {
                // 新路径直接发送当前冻结候选。
                requestPackBeatmapTo(std::move(filePath));
            }
        }
        // 无论确认还是取消，Display 完成本轮都关闭对话框状态。
        ImGuiFileDialog::Instance()->Close();
    }
}

/// @brief 渲染统一 RM/IMD 资源包输出文件选择器。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 热路径：仅在统一文件选择器打开时绘制。
/// @note 流程固定输出 ZIP，并复用同一覆盖确认弹窗。
/// @warning 文件存在性只在用户确认分支查询，不进入空闲帧热路径。
/// @pre ImdPackageFilePicker 已由对应打开入口创建。
void PackBeatmapAction::renderImdPackageOutputFileDialog(float dpiScale)
{
    // 统一选择器使用与常规包相同的居中样式。
    Utils::CenteredModalPopupScope fileDialogStyle(dpiScale);
    if ( ImGuiFileDialog::Instance()->IsOpened("ImdPackageFilePicker") ) {
        Utils::prepareCenteredModalWindow({ 600, 400 });
    }
    if ( ImGuiFileDialog::Instance()->Display(
             "ImdPackageFilePicker",
             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoSavedSettings,
             { 600, 400 }) ) {
        if ( ImGuiFileDialog::Instance()->IsOk() ) {
            // 统一选择器返回值仍需替换扩展名以防手工输入偏离过滤器。
            auto outputPath = Config::utf8ToPath(
                ImGuiFileDialog::Instance()->GetFilePathName());
            outputPath.replace_extension(".zip");
            std::string filePath = Config::pathToUtf8(outputPath);

            // 更新全局最近目录，不改变其余编辑器设置。
            auto& engine       = Logic::EditorEngine::instance();
            auto  editorConfig = engine.getEditorConfig();
            editorConfig.settings.lastFilePickerPath =
                ImGuiFileDialog::Instance()->GetCurrentPath();
            engine.setEditorConfig(editorConfig);

            // error_code 查询保持禁异常约束，并区分有效存在结果。
            std::error_code filesystemError;
            if ( std::filesystem::exists(outputPath, filesystemError) &&
                 !filesystemError ) {
                m_pendingPackageOverwritePath  = std::move(filePath);
                m_pendingOverwriteIsImdPackage = true;
                // 下一帧打开共用覆盖确认弹窗。
                m_showPackageOverwriteWarning = true;
            } else {
                // 新路径立即分发独立资源包导出。
                requestExportImdPackageTo(std::move(filePath));
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }
}

/// @brief 渲染打包输出覆盖确认弹窗。
/// @param dpiScale 当前窗口内容缩放。
/// @warning UI 热路径：只在存在待确认覆盖目标时绘制固定内容。
/// @note pendingOverwriteIsImdPackage 决定确认后分发的逻辑命令类型。
/// @details 取消只清理临时目标；确认把同一路径交给对应逻辑命令，由逻辑层
/// 执行实际覆盖写入，因此本函数不直接删除既有文件。
void PackBeatmapAction::renderPackageOverwriteWarningPopup(float dpiScale)
{
    // 固定内部 ID 保持覆盖弹窗身份稳定。
    constexpr const char* popupId =
        "确认覆盖打包文件###PackageOverwriteWarningModal";
    if ( m_showPackageOverwriteWarning ) {
        // 一次性请求转交 ImGui 弹窗栈。
        ::MMM::UI::FeedbackOpenPopup(popupId);
        m_showPackageOverwriteWarning = false;
    }

    // 没有打开的覆盖请求时不创建样式作用域。
    if ( !ImGui::IsPopupOpen(popupId) ) return;

    Utils::CenteredModalPopupScope popupStyle(dpiScale);
    if ( popupStyle.begin(popupId,
                          nullptr,
                          ImGuiWindowFlags_None,
                          ImVec2(540.0f * dpiScale, 0.0f)) ) {
        ImGui::TextWrapped("目标打包文件已经存在，是否覆盖？");
        if ( !m_pendingPackageOverwritePath.empty() ) {
            // 展示完整目标路径，用户可在确认前核对覆盖对象。
            ImGui::Spacing();
            ImGui::TextWrapped("目标文件：%s",
                               m_pendingPackageOverwritePath.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const ImVec2 buttonSize(120.0f * dpiScale, 0.0f);
        if ( ::MMM::UI::FeedbackButton("确认覆盖", buttonSize) ) {
            // 根据流程来源选择常规包或独立 RM/IMD 命令。
            if ( m_pendingOverwriteIsImdPackage ) {
                requestExportImdPackageTo(m_pendingPackageOverwritePath);
            } else {
                requestPackBeatmapTo(m_pendingPackageOverwritePath);
            }
            // 分发后立即清空待确认状态，防止重复确认。
            m_pendingPackageOverwritePath.clear();
            m_pendingOverwriteIsImdPackage = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                       buttonSize) ) {
            // 取消只丢弃输出目标，不删除或覆盖现有文件。
            m_pendingPackageOverwritePath.clear();
            m_pendingOverwriteIsImdPackage = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

/// @brief 创建打开谱面打包流程的菜单项业务处理器。
/// @return 由主菜单注册表独占持有的动作实例。
/// @note 工厂每次创建独立状态机，弹窗状态不会跨菜单实例共享。
/// @warning 返回对象应持续存活以维护跨帧模态窗口状态。
/// @note 销毁动作会同时丢弃尚未确认的临时 UI 状态。
std::unique_ptr<IMainMenuItemActionHandler> createPackBeatmapAction()
{
    // unique_ptr 与动作注册接口的所有权约定一致。
    return std::make_unique<PackBeatmapAction>();
}

}  // namespace MMM::UI
