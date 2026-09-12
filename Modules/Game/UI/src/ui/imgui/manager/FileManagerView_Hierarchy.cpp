#ifndef IMGUI_DEFINE_MATH_OPERATORS
#    define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/imgui/manager/FileManagerView.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/DesktopPathUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fmt/format.h>
#include <imgui_internal.h>
#include <optional>
#include <system_error>

namespace MMM::UI
{
namespace
{
/// @brief 表格列编号。
///
/// 枚举值与 ImGui::TableSetupColumn 和排序规格的 ColumnIndex 保持一一对应。
/// 名称列是树导航主列，其余三列只提供元数据展示。
/// 新增列时必须同步更新 setup、标签数组和排序映射。
/// 枚举不作为持久化文件格式写盘。
enum FileTableColumn : int {
    /// @brief 名称列。
    FileTableColumnName = 0,

    /// @brief 类型列。
    FileTableColumnType = 1,

    /// @brief 大小列。
    FileTableColumnSize = 2,

    /// @brief 修改时间列。
    FileTableColumnModifiedTime = 3
};

/// @brief 将 ASCII 字符串转换为小写，用于稳定排序。
/// @param value 输入字符串。
/// @return 小写后的字符串。
///
/// 仅转换 ASCII A-Z，非 ASCII UTF-8 字节保持原样；用于扩展名与稳定回退排序，
/// 不承担自然语言大小写折叠。
std::string toLowerAscii(std::string value)
{
    // 原地转换复用调用方传入副本的存储。
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            if ( ch >= 'A' && ch <= 'Z' ) {
                // 显式范围运算避免受当前 C locale 影响。
                return static_cast<char>(ch - 'A' + 'a');
            }
            // UTF-8 多字节和标点原样保留。
            return static_cast<char>(ch);
        });
    return value;
}

/// @brief 判断扩展名是否为谱面文件。
/// @param extension UTF-8 扩展名。
/// @return 是支持的谱面扩展名时返回 true。
///
/// 白名单用于决定文件激活行为，实际解析格式仍由编辑器导入链验证。
bool isBeatmapExtension(const std::string& extension)
{
    // 扩展名比较大小写不敏感，并要求保留前导点。
    const auto ext = toLowerAscii(extension);
    return ext == ".osu" || ext == ".imd" || ext == ".mc" || ext == ".mmm";
}

/// @brief 判断扩展名是否为音频文件。
/// @param extension UTF-8 扩展名。
/// @return 是支持的音频扩展名时返回 true。
///
/// 该集合同时用于图标识别和删除目录前的受管音频风险扫描。
bool isAudioExtension(const std::string& extension)
{
    // 扩展名白名单与项目音频导入入口保持一致。
    const auto ext = toLowerAscii(extension);
    return ext == ".mp3" || ext == ".wav" || ext == ".ogg" || ext == ".flac" ||
           ext == ".opus" || ext == ".aac" || ext == ".m4a";
}

/// @brief 判断文件系统条目是否是项目内部隐藏配置目录。
/// @param path 候选文件系统条目。
/// @return basename 精确等于 .mmm 时返回 true。
///
/// 项目存储目录包含内部元数据，不应出现在普通资源树或目录项目数量中。
bool isInternalProjectStorageEntry(const std::filesystem::path& path)
{
    // 只比较最后一个路径分量，父目录中出现同名片段不受影响。
    return path.filename() == std::filesystem::path(".mmm");
}

/// @brief 文件或目录移动后同步其中音频资源的项目路径。
/// @param oldPath 移动前路径。
/// @param newPath 移动后路径。
/// @return 成功时为空；写盘失败并回滚时返回面向用户的错误。
///
/// EditorEngine 负责识别 oldPath 子树下受管音频、更新项目资源路径并在保存失败时
/// 回滚；此 helper 只把错误传给文件管理器弹窗并记录成功数量。
/// @warning 低频文件操作路径：只在用户确认移动或重命名后扫描项目音频资源。
std::string syncMovedProjectAudioResourcePaths(
    const std::filesystem::path& oldPath, const std::filesystem::path& newPath)
{
    auto&       engine = Logic::EditorEngine::instance();
    std::string errorMessage;
    // 由逻辑层集中维护资源和谱面引用的一致性。
    const auto changedCount = engine.remapAudioResourcePathsAfterMove(
        oldPath, newPath, &errorMessage);
    // 非空错误表示逻辑层已经执行失败处理，调用方不得继续刷新为成功。
    if ( !errorMessage.empty() ) return errorMessage;
    // 没有受管音频时文件操作本身仍视为成功。
    if ( changedCount == 0 ) return {};

    XINFO("Updated {} project audio resource path(s) after move", changedCount);
    return {};
}

/// @brief 计算目录直属项目数量。
/// @param path 需要统计的目录。
/// @return 成功时返回数量；失败时为空。
///
/// 只统计直属条目并排除内部 .mmm 存储目录。使用 error_code 迭代，无法完整读取时
/// 返回 nullopt，避免 UI 把部分计数误当作准确结果。
/// @warning 目录快照重建路径：每个目录缓存失效时至多调用一次。
std::optional<std::uintmax_t> countDirectoryChildren(
    const std::filesystem::path& path)
{
    // skip_permission_denied 允许遍历器跳过无权限子项，但构造错误仍报告未知。
    std::error_code filesystemError;
    constexpr auto  options =
        std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::directory_iterator iterator(
        path, options, filesystemError);
    if ( filesystemError ) {
        // 构造失败时没有可靠的起始迭代范围。
        return std::nullopt;
    }

    std::uintmax_t                            count = 0;
    const std::filesystem::directory_iterator endIterator;
    // 内部存储目录不属于用户可操作资源数量。
    while ( iterator != endIterator ) {
        if ( !isInternalProjectStorageEntry(iterator->path()) ) {
            ++count;
        }
        // 显式 increment 捕获迭代中途发生的文件系统错误。
        iterator.increment(filesystemError);
        if ( filesystemError ) {
            return std::nullopt;
        }
    }
    return count;
}

/// @brief 生成文件大小显示文本。
/// @param size 文件字节数。
/// @return 适合列表展示的大小文本。
///
/// 阈值采用二进制 1024 进位，并通过翻译格式键决定单位和小数显示；字节区间保留
/// 整数精度，更大区间转换为 double 仅用于展示。
std::string formatFileSize(std::uintmax_t size)
{
    // 三个常量以 double 表示，便于后续直接除法格式化。
    constexpr double kibi = 1024.0;
    constexpr double mebi = kibi * 1024.0;
    constexpr double gibi = mebi * 1024.0;

    if ( size < 1024 ) {
        // 小文件显示精确字节数。
        return TR_FMT("ui.file_manager.size_bytes", size);
    }
    const double value = static_cast<double>(size);
    if ( value < mebi ) {
        // KiB 区间不跨越 MiB 阈值。
        return TR_FMT("ui.file_manager.size_kib", value / kibi);
    }
    if ( value < gibi ) {
        // MiB 区间与 GiB 使用相同本地化格式策略。
        return TR_FMT("ui.file_manager.size_mib", value / mebi);
    }
    return TR_FMT("ui.file_manager.size_gib", value / gibi);
}

/// @brief 生成大小列显示文本。
/// @param entry 文件树条目。
/// @return 目录项目数或文件大小文本。
///
/// 目录的“大小”列展示直属可见项目数，普通文件展示字节大小；任一文件系统查询
/// 失败都以统一未知占位表达，不伪造零值。
std::string formatSizeColumn(const FileManagerView::DirectoryEntryInfo& entry)
{
    if ( entry.isDirectory ) {
        // 目录计数是否有效由快照构建阶段单独记录。
        if ( !entry.hasDirectoryChildCount ) {
            return TR("ui.file_manager.value_unknown").data();
        }
        // 目录数量使用复数规则由翻译系统格式化。
        return TR_FMT("ui.file_manager.directory_item_count",
                      entry.directoryChildCount);
    }

    if ( !entry.hasFileSize ) {
        // 权限或状态错误显示未知，而不是误导为零字节。
        return TR("ui.file_manager.value_unknown").data();
    }
    return formatFileSize(entry.fileSize);
}

/// @brief 生成类型列显示文本。
/// @param entry 文件树条目。
/// @return 目录、扩展名或普通文件类型文本。
///
/// 目录使用本地化名称；文件优先显示去掉点号的小写扩展名，没有扩展名时回退普通
/// 文件标签。这里只影响展示，不改变激活或导入分类。
std::string formatTypeColumn(const FileManagerView::DirectoryEntryInfo& entry)
{
    if ( entry.isDirectory ) {
        // 目录类型不依赖平台文件属性文本。
        return TR("ui.file_manager.type_directory").data();
    }
    if ( entry.extension.empty() ) {
        // 无扩展文件统一归为普通文件。
        return TR("ui.file_manager.type_file").data();
    }
    std::string extension = entry.extension;
    if ( extension.size() > 1 && extension.front() == '.' ) {
        // 只移除标准扩展名的第一个点号。
        extension.erase(extension.begin());
    }
    return extension.empty() ? TR("ui.file_manager.type_file").data()
                             : toLowerAscii(extension);
}

/// @brief 将文件系统时间转换为本地时间文本。
/// @param time 文件系统时间。
/// @return 本地时间文本，格式为 yyyy/mm/dd HH:MM。
///
/// C++ 文件时钟与 system_clock 纪元可能不同，先用两种 clock
/// 的当前差值近似转换， 再调用线程安全平台本地时间
/// API。格式化失败返回统一未知占位。
std::string formatModifiedTime(std::filesystem::file_time_type time)
{
    // 以当前时刻对齐两个时钟，避免假设实现使用相同 epoch。
    const auto systemTime =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            time - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now());
    // time_t 供平台 localtime_s/localtime_r 接口使用。
    const std::time_t timeValue =
        std::chrono::system_clock::to_time_t(systemTime);

    std::tm localTime{};
#ifdef _WIN32
    // Windows 安全版本参数顺序为目标在前。
    localtime_s(&localTime, &timeValue);
#else
    // POSIX 可重入版本不使用进程级静态 tm 缓冲。
    localtime_r(&timeValue, &localTime);
#endif

    // 固定格式最大长度远小于 32 字节缓冲。
    char buffer[32]{};
    if ( std::strftime(buffer, sizeof(buffer), "%Y/%m/%d %H:%M", &localTime) ==
         0 ) {
        // 零返回表示缓冲不足或格式化失败。
        return TR("ui.file_manager.value_unknown").data();
    }
    return buffer;
}

/// @brief 生成修改时间列显示文本。
/// @param entry 文件树条目。
/// @return 本地修改时间或未知占位。
///
/// hasLastWriteTime
/// 区分真实时间值与查询失败，避免默认构造时间被显示为有效日期。
std::string formatModifiedColumn(
    const FileManagerView::DirectoryEntryInfo& entry)
{
    if ( !entry.hasLastWriteTime ) {
        // 快照构建失败时统一显示未知。
        return TR("ui.file_manager.value_unknown").data();
    }
    return formatModifiedTime(entry.lastWriteTime);
}

/// @brief 选择文件树条目图标。
/// @param entry 文件树条目。
/// @return 图标 UTF-8 文本。
///
/// 目录优先于扩展名；音频使用音乐图标，其余文件共享普通文件图标。谱面目前不在
/// 图标层单独区分，激活行为仍按扩展名识别。
const char* fileEntryIcon(const FileManagerView::DirectoryEntryInfo& entry)
{
    if ( entry.isDirectory ) {
        // 即使目录名带音频后缀也必须展示文件夹图标。
        return ICON_MMM_FOLDER;
    }
    if ( isAudioExtension(entry.extension) ) {
        // 音轨资源在文件树中提供直接视觉提示。
        return ICON_MMM_MUSIC;
    }
    return ICON_MMM_FILE;
}

/// @brief 生成目录缓存键。
/// @param path 目录路径。
/// @return UTF-8 缓存键。
///
/// 词法规整消除点号和重复分隔符，但不要求路径存在，因此缓存失效也可处理刚删除的
/// 目录。UTF-8 转换保证跨平台 map 键类型一致。
std::string directoryCacheKey(const std::filesystem::path& path)
{
    // 缓存键不解析符号链接，保留用户浏览树的词法身份。
    return Config::pathToUtf8(path.lexically_normal());
}

/// @brief 比较两个可选数值。
/// @tparam T 可比较数值类型。
/// @param lhs 左值。
/// @param lhsValid 左值是否有效。
/// @param rhs 右值。
/// @param rhsValid 右值是否有效。
/// @return 小于返回 -1，大于返回 1，相等返回 0。
///
/// 有效值始终排在未知值之前；双方未知视为相等并交给文件名或完整路径回退排序。
template<typename T>
int compareOptionalValue(const T& lhs, bool lhsValid, const T& rhs,
                         bool rhsValid)
{
    if ( lhsValid != rhsValid ) {
        // 升序基础比较中有效项优先，降序最终会整体反转。
        return lhsValid ? -1 : 1;
    }
    if ( !lhsValid ) {
        // 双方都无有效值，没有可比较的数据。
        return 0;
    }
    // 使用严格弱序所需的双向小于比较，不要求 T 定义三路比较。
    if ( lhs < rhs ) return -1;
    if ( rhs < lhs ) return 1;
    return 0;
}

/// @brief 比较两个文件树条目。
/// @param lhs 左条目。
/// @param rhs 右条目。
/// @param sortKey 排序字段。
/// @param sortDirection 排序方向。
/// @param directoriesFirst 是否目录优先。
/// @return lhs 应排在 rhs 之前时返回 true。
///
/// 目录优先是独立于列方向的首要分组；组内先比较用户选择列，再以大小写不敏感
/// 文件名和完整路径建立确定性全序。未知数值通过 compareOptionalValue
/// 排在有效值后。
bool fileEntryLess(const FileManagerView::DirectoryEntryInfo& lhs,
                   const FileManagerView::DirectoryEntryInfo& rhs,
                   FileManagerView::FileSortKey               sortKey,
                   FileManagerView::SortDirection             sortDirection,
                   bool                                       directoriesFirst)
{
    if ( directoriesFirst && lhs.isDirectory != rhs.isDirectory ) {
        // 目录分组不随升降序反转，始终位于普通文件之前。
        return lhs.isDirectory;
    }

    // compareResult 使用负、零、正表达三态，最后统一处理方向。
    int compareResult = 0;
    switch ( sortKey ) {
    case FileManagerView::FileSortKey::Name:
        // 名称排序只折叠 ASCII 大小写，保留 UTF-8 字节稳定性。
        compareResult =
            toLowerAscii(lhs.filename).compare(toLowerAscii(rhs.filename));
        break;
    case FileManagerView::FileSortKey::Type:
        // 类型列按原始扩展名排序，显示格式不参与比较。
        compareResult =
            toLowerAscii(lhs.extension).compare(toLowerAscii(rhs.extension));
        break;
    case FileManagerView::FileSortKey::Size:
        if ( lhs.isDirectory || rhs.isDirectory ) {
            // 目录以直属可见条目数量作为大小语义。
            compareResult = compareOptionalValue(lhs.directoryChildCount,
                                                 lhs.hasDirectoryChildCount,
                                                 rhs.directoryChildCount,
                                                 rhs.hasDirectoryChildCount);
        } else {
            // 普通文件使用字节数和独立有效位。
            compareResult = compareOptionalValue(
                lhs.fileSize, lhs.hasFileSize, rhs.fileSize, rhs.hasFileSize);
        }
        break;
    case FileManagerView::FileSortKey::ModifiedTime:
        // file_time_type 可直接通过 operator< 建立顺序。
        compareResult = compareOptionalValue(lhs.lastWriteTime,
                                             lhs.hasLastWriteTime,
                                             rhs.lastWriteTime,
                                             rhs.hasLastWriteTime);
        break;
    }

    if ( compareResult == 0 ) {
        // 主列相同时按名称稳定排列，便于用户定位。
        compareResult =
            toLowerAscii(lhs.filename).compare(toLowerAscii(rhs.filename));
    }
    if ( compareResult == 0 ) {
        // 同名条目最后以完整路径打破平局，满足严格弱序。
        compareResult =
            toLowerAscii(lhs.fullPath).compare(toLowerAscii(rhs.fullPath));
    }

    if ( sortDirection == FileManagerView::SortDirection::Descending ) {
        // 只反转组内比较，目录优先分组已在函数开头返回。
        compareResult = -compareResult;
    }
    return compareResult < 0;
}

/// @brief 组合排序菜单项显示文本。
/// @param columnLabel 排序字段显示名。
/// @param direction 排序方向。
/// @return 带方向后缀的菜单文本。
///
/// 菜单文本完全由翻译格式键构造，不作为 ImGui 稳定 ID；调用点另行提供选择状态。
std::string makeSortMenuLabel(const char*                    columnLabel,
                              FileManagerView::SortDirection direction)
{
    return direction == FileManagerView::SortDirection::Ascending
               ? TR_FMT("ui.resource_table.sort_ascending_fmt", columnLabel)
               : TR_FMT("ui.resource_table.sort_descending_fmt", columnLabel);
}

/// @brief 查询表格列当前是否有效显示。
/// @param table ImGui 表格指针。
/// @param column 列索引。
/// @return 当前帧列有效显示时返回 true。
///
/// IsEnabled 是 ImGui 完成本帧设置后的实际状态，区别于用户请求的下一帧状态。
bool isTableColumnEnabled(const ImGuiTable* table, int column)
{
    // 同时校验表指针和索引，避免访问内部 Columns 越界。
    return table && column >= 0 && column < table->ColumnsCount &&
           table->Columns[column].IsEnabled;
}

/// @brief 查询表格列的用户显隐状态。
/// @param table ImGui 表格指针。
/// @param column 列索引。
/// @return 用户设置为显示时返回 true。
///
/// IsUserEnabled 表示配置层选择，可能在本帧布局裁剪下与 IsEnabled 不同。
bool isTableColumnUserEnabled(const ImGuiTable* table, int column)
{
    // 该 helper 只读内部状态，不排队任何布局修改。
    return table && column >= 0 && column < table->ColumnsCount &&
           table->Columns[column].IsUserEnabled;
}

/// @brief 排队设置表格列下一帧的用户显隐状态。
/// @param table ImGui 表格指针。
/// @param column 列索引。
/// @param enabled 是否显示。
///
/// ImGui 表格列不能在当前布局中安全即时切换，因此写入 IsUserEnabledNextFrame，
/// 由下一帧 TableBeginApplyRequests 统一应用。
void queueTableColumnEnabled(ImGuiTable* table, int column, bool enabled)
{
    if ( !table || column < 0 || column >= table->ColumnsCount ) {
        // 没有当前表或错误列号时忽略菜单请求。
        return;
    }
    table->Columns[column].IsUserEnabledNextFrame = enabled;
}

/// @brief 判断路径是否存在。
/// @param path 需要检查的路径。
/// @return 存在且没有文件系统错误时返回 true。
///
/// error_code 版本把权限和状态查询失败统一视为不存在，供冲突检查与 UI
/// 守卫使用。
bool filesystemPathExists(const std::filesystem::path& path)
{
    // 错误与 false 结果必须同时纳入返回值。
    std::error_code filesystemError;
    const bool      exists = std::filesystem::exists(path, filesystemError);
    return exists && !filesystemError;
}

/// @brief 判断路径是否为目录。
/// @param path 需要检查的路径。
/// @return 是目录且没有文件系统错误时返回 true。
///
/// 不跟随异常路径，查询失败按非目录处理，调用方会显示通用文件系统错误。
bool filesystemPathIsDirectory(const std::filesystem::path& path)
{
    // 使用 error_code 遵守项目禁异常约束。
    std::error_code filesystemError;
    const bool      isDirectory =
        std::filesystem::is_directory(path, filesystemError);
    return isDirectory && !filesystemError;
}

/// @brief 获取用于父子关系判断的规范化路径。
/// @param path 输入路径。
/// @return 尽量规范化后的路径。
///
/// weakly_canonical 解析存在前缀和符号链接，适合文件操作边界判断；路径不存在或
/// 查询失败时回退词法规整，仍可用于确定性比较。
std::filesystem::path comparableFilesystemPath(
    const std::filesystem::path& path)
{
    // 不抛异常的规范化尝试优先取得真实父子关系。
    std::error_code filesystemError;
    auto            canonicalPath =
        std::filesystem::weakly_canonical(path, filesystemError);
    if ( !filesystemError ) {
        // 再词法规整清除实现可能保留的冗余分量。
        return canonicalPath.lexically_normal();
    }
    // 失败回退不保证解析符号链接，仅作为保守比较键。
    return path.lexically_normal();
}

/// @brief 判断路径是否位于指定根目录内。
/// @param root 根目录。
/// @param path 待检查路径。
/// @return path 等于 root 或位于 root 内时返回 true。
///
/// 通过路径分量逐个比较而非字符串前缀，避免 /project2 被误判为 /project
/// 子路径。 root 自身全部匹配后即成立，允许粘贴或操作项目根本身的显式检查调用。
bool pathIsInsideOrSame(const std::filesystem::path& root,
                        const std::filesystem::path& path)
{
    // 双方使用同一种规范化策略处理符号链接和点号分量。
    const auto normalizedRoot = comparableFilesystemPath(root);
    const auto normalizedPath = comparableFilesystemPath(path);
    auto       rootIt         = normalizedRoot.begin();
    auto       pathIt         = normalizedPath.begin();
    for ( ; rootIt != normalizedRoot.end(); ++rootIt, ++pathIt ) {
        // 目标提前结束或任一分量不同都说明不在根子树中。
        if ( pathIt == normalizedPath.end() || *rootIt != *pathIt ) {
            return false;
        }
    }
    // 根的全部分量匹配，目标可等于根或包含更多子分量。
    return true;
}

/// @brief 解析项目音频资源对应的规范化绝对路径。
/// @param project 音频资源所属项目。
/// @param resource 待解析的音频资源。
/// @return 可与文件树条目稳定比较的绝对路径。
///
/// 新项目通常保存根相对路径，旧项目可能保存绝对路径或重复项目文件夹前缀。本函数
/// 依次处理三种表示，并始终返回 comparableFilesystemPath 结果。
std::filesystem::path resolveProjectAudioResourcePath(
    const Project& project, const AudioResource& resource)
{
    // 先把 UTF-8 持久化值恢复为平台路径并词法规整。
    auto storedPath = Config::utf8ToPath(resource.m_path).lexically_normal();
    if ( storedPath.is_absolute() ) {
        // 绝对旧路径直接规范化，不重复拼接项目根。
        return comparableFilesystemPath(storedPath);
    }

    // 常规表示是项目根加资源相对路径。
    const auto projectRoot = comparableFilesystemPath(project.m_projectRoot);
    auto       directPath  = projectRoot / storedPath;
    if ( filesystemPathExists(directPath) ) {
        // 目标存在时常规解析最可信。
        return comparableFilesystemPath(directPath);
    }

    // 兼容相对路径首分量重复项目文件夹名的历史数据。
    auto iterator = storedPath.begin();
    if ( iterator != storedPath.end() && *iterator == projectRoot.filename() ) {
        std::filesystem::path strippedPath;
        ++iterator;
        for ( ; iterator != storedPath.end(); ++iterator ) {
            strippedPath /= *iterator;
        }
        if ( !strippedPath.empty() ) {
            // 即使目标当前缺失，也返回修正后的比较位置。
            return comparableFilesystemPath(projectRoot / strippedPath);
        }
    }
    // 没有兼容前缀时保留常规拼接结果用于比较。
    return comparableFilesystemPath(directPath);
}

/// @brief 按文件树绝对路径查找项目音频资源。
/// @param project 当前项目。
/// @param path 待匹配的音频文件。
/// @return 精确路径匹配的资源；未登记时返回空。
///
/// 每个资源先解析历史路径表示，再与文件树绝对路径比较；返回指针观察 project
/// 资源 vector 元素，仅在集合未修改期间有效。
const AudioResource* findProjectAudioResourceAtPath(
    const Project& project, const std::filesystem::path& path)
{
    // 目标也采用相同真实/词法规整策略。
    const auto comparablePath = comparableFilesystemPath(path);
    // 精确路径匹配避免同名不同目录音频被误激活。
    const auto iterator =
        std::find_if(project.m_audioResources.begin(),
                     project.m_audioResources.end(),
                     [&](const AudioResource& resource) {
                         return resolveProjectAudioResourcePath(
                                    project, resource) == comparablePath;
                     });
    // 不复制资源对象，由调用方立即读取其稳定 ID。
    return iterator == project.m_audioResources.end() ? nullptr : &*iterator;
}

/// @brief 检查目录树中是否包含受项目管理的音频文件类型。
/// @param directory 待检查目录。
/// @param filesystemError 接收目录遍历错误。
/// @return 成功时返回是否存在音频文件，失败时返回空。
///
/// 删除目录前需要知道是否可能影响项目受管音轨。扫描只按扩展名识别风险，发现
/// 第一个音频文件即可提前返回；任何遍历错误返回 nullopt 供确认流程保守拒绝。
/// @warning 低频删除确认路径：会递归扫描用户明确请求删除的单个目录。
std::optional<bool> directoryContainsAudioFile(
    const std::filesystem::path& directory, std::error_code& filesystemError)
{
    // 跳过权限不足目录，但遍历器本身的错误仍通过 error_code 报告。
    constexpr auto options =
        std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator iterator(
        directory, options, filesystemError);
    if ( filesystemError ) return std::nullopt;

    const std::filesystem::recursive_directory_iterator endIterator;
    while ( iterator != endIterator ) {
        // 状态查询失败无法证明目录安全，因此终止并返回未知。
        const bool isRegularFile = iterator->is_regular_file(filesystemError);
        if ( filesystemError ) return std::nullopt;
        if ( isRegularFile && isAudioExtension(Config::pathToUtf8(
                                  iterator->path().extension())) ) {
            // 风险判断只需存在性，不需要继续统计。
            return true;
        }
        // 显式递增捕获中途删除、权限变化等错误。
        iterator.increment(filesystemError);
        if ( filesystemError ) return std::nullopt;
    }
    return false;
}

/// @brief 判断文件名输入是否包含路径分隔符。
/// @param filename UTF-8 文件名输入。
/// @return 含路径分隔符时返回 true。
///
/// 同时拒绝 POSIX 与 Windows
/// 分隔符，防止跨平台输入借重命名或新建目录越出目标父级。
bool containsPathSeparator(const std::string& filename)
{
    // 使用字节查找即可识别两个 ASCII 分隔符。
    return filename.find('/') != std::string::npos ||
           filename.find('\\') != std::string::npos;
}

/// @brief 判断文件名输入是否可用于单个文件系统条目。
/// @param filename UTF-8 文件名输入。
/// @return 文件名有效时返回 true。
///
/// 文件名必须非空、不是当前或父目录特殊分量，并且不能包含任一平台路径分隔符。
/// 平台保留名和字符由实际 filesystem 操作通过 error_code 报告。
bool isValidSingleFilename(const std::string& filename)
{
    if ( filename.empty() || filename == "." || filename == ".." ) {
        // 三种值无法表示一个新的直属条目。
        return false;
    }
    // 分隔符检查阻止把输入解释成嵌套或根外路径。
    return !containsPathSeparator(filename);
}

/// @brief 为复制或移动生成不覆盖既有文件的目标路径。
/// @param source 源路径。
/// @param targetDirectory 目标目录。
/// @return 唯一目标路径。
///
/// 首选保留原文件名；冲突时对目录追加完整后缀，对文件在 stem 与 extension 之间
/// 追加本地化 copy 后缀。最多尝试 9999 个编号，最后返回英文兜底候选。
/// @warning 低频文件操作路径：每次粘贴前执行有限次 exists 查询。
std::filesystem::path makeUniqueDestinationPath(
    const std::filesystem::path& source,
    const std::filesystem::path& targetDirectory)
{
    // 目录没有文件扩展语义，文件则需保留原后缀。
    const bool sourceIsDirectory = filesystemPathIsDirectory(source);
    const auto filename          = source.filename();
    auto       candidate         = targetDirectory / filename;
    if ( !filesystemPathExists(candidate) ) {
        // 无冲突时完全保留原 basename。
        return candidate;
    }

    // 文件 stem 与 extension 分开，确保副本后缀位于扩展名前。
    const auto filenameText = Config::pathToUtf8(filename);
    const auto stemText =
        sourceIsDirectory ? filenameText : Config::pathToUtf8(source.stem());
    const auto extensionText = sourceIsDirectory
                                   ? std::string{}
                                   : Config::pathToUtf8(source.extension());
    for ( int index = 1; index < 10000; ++index ) {
        // 第一个副本使用简短翻译，之后加入递增编号。
        const std::string suffix =
            index == 1 ? TR("ui.file_manager.copy_suffix").data()
                       : TR_FMT("ui.file_manager.copy_suffix_numbered", index);
        // UTF-8 名称通过路径 helper 恢复为平台原生表示。
        candidate = targetDirectory /
                    Config::utf8ToPath(stemText + suffix + extensionText);
        if ( !filesystemPathExists(candidate) ) {
            // 找到第一个空闲名称立即返回，避免覆盖既有文件。
            return candidate;
        }
    }
    // 极端冲突数量下提供确定性兜底；实际 copy 仍以不覆盖模式报告冲突。
    return targetDirectory /
           Config::utf8ToPath(stemText + " copy" + extensionText);
}

/// @brief 将文件系统错误格式化为用户可读文本。
/// @param action 操作名称。
/// @param error 文件系统错误码。
/// @return 格式化后的错误文本。
///
/// 操作名称由调用点本地化传入；有效 error_code 使用系统消息，无错误码时使用项目
/// 的未知错误翻译，确保所有失败弹窗都有内容。
std::string formatFilesystemError(const char*            action,
                                  const std::error_code& error)
{
    // 翻译格式统一组合动作和底层原因。
    return TR_FMT(
        "ui.file_manager.operation_error_fmt",
        action,
        error ? error.message() : TR("ui.file_manager.error_unknown").data());
}

/// @brief 复制文件或目录。
/// @param source 源路径。
/// @param destination 目标路径。
/// @param error 输出文件系统错误码。
/// @return 成功时返回 true。
///
/// std::filesystem::copy 使用 recursive 统一处理文件和目录，且不指定
/// overwrite， 因此目标冲突不会破坏既有数据。调用前通常已由
/// makeUniqueDestinationPath 避让。
/// @warning 低频文件操作路径：可能递归复制目录，只由用户确认粘贴触发。
bool copyFilesystemEntry(const std::filesystem::path& source,
                         const std::filesystem::path& destination,
                         std::error_code&             error)
{
    // 清除调用方复用的旧错误，返回值只反映本次复制。
    error.clear();
    // error_code 重载遵守项目禁异常约束。
    std::filesystem::copy(
        source, destination, std::filesystem::copy_options::recursive, error);
    // 任一递归复制错误均作为整体失败反馈给用户。
    return !error;
}
}  // namespace

/// @brief 绘制当前项目根标题和可排序的递归文件树。
/// @param layoutContext Clay 提供的可用区域与布局上下文。
/// @param sourceManager 用于文件激活和打开新谱面向导。
///
/// Clay 负责根标题与表格的纵向分配，ImGui
/// 表格提供冻结表头、列显隐、排序和滚动。 列宽按当前字体、DPI
/// 和容器宽度计算，名称列吸收剩余空间，修改时间列可拉伸。
/// @warning UI 热路径：每帧绘制；文件系统访问由目录快照缓存限制在失效重建时。
void FileManagerView::renderActiveProjectView(LayoutContext& layoutContext,
                                              UIManager*     sourceManager)
{
    // 皮肤配置由后续布局/控件使用，引用只覆盖本帧。
    auto& skinCfg = Config::SkinManager::instance();

    // 固定本帧项目根副本，避免 lambda 捕获可变成员引用。
    const std::filesystem::path projectRoot = m_currentRoot;

    // DPI 下限为一，避免异常配置缩小触控与行高。
    const float dpiScale =
        std::max(1.0f, Config::AppConfig::instance().getWindowContentScale());
    const auto& style = ImGui::GetStyle();
    // Clay 尺寸使用 uint16_t，先夹非负再向上取整。
    auto toLayoutPixels = [](float value) {
        return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
    };
    // 与谱面管理器相邻项布局一致，避免 Header 和表格过近。
    const uint16_t itemSpacing = toLayoutPixels(style.ItemSpacing.y);
    // 根内边距随 DPI 增长，同时受当前宽度百分比限制。
    const uint16_t rootPadding = toLayoutPixels(std::min(
        4.0f * dpiScale, std::max(0.0f, layoutContext.m_avail.x) * 0.02f));

    // treeVBox 在根标题关闭时只保留 Header 高度。
    CLayVBox treeVBox;
    treeVBox.setSpacing(itemSpacing);

    // 项目根作为 CollapsingHeader，控制整个文件表格显隐。
    treeVBox.addElement(
        "ProjectRootHeader",
        Sizing::Grow(),
        Sizing::Fixed(ImGui::GetFrameHeight()),
        [this, projectRoot](Clay_BoundingBox r, bool isHovered) {
            // 可见名只展示根目录 basename，完整路径放入 tooltip。
            std::string rootName = Config::pathToUtf8(projectRoot.filename());
            Utils::renderScrollingCollapsingHeader(
                "ProjectRootHeader", rootName, &m_showRoot, r);
            if ( ImGui::IsItemHovered() ) {
                // tooltip 帮助区分同名但位于不同位置的项目。
                std::string fullPath = Config::pathToUtf8(projectRoot);
                ImGui::SetTooltip("%s", fullPath.c_str());
            }
        });

    if ( m_showRoot ) {
        // 展开根节点后让表格占满剩余纵向空间。
        treeVBox.addElement(
            "FileTree",
            Sizing::Grow(),
            Sizing::Grow(),
            [this, sourceManager, dpiScale](Clay_BoundingBox r,
                                            bool             isHovered) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                {
                    // 滚动条样式作用域只覆盖文件表格。
                    Utils::VerticalScrollbarStyleScope verticalScrollbarStyle(
                        dpiScale);
                    // 表格允许用户调整、重排、隐藏和排序列，并在内部纵向滚动。
                    const ImGuiTableFlags tableFlags =
                        ImGuiTableFlags_BordersV |
                        ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_Resizable |
                        ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                        ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_SizingStretchProp;
                    // 宽度下限结合字体缩放，避免翻译或大字体被裁切。
                    const auto& tableStyle = ImGui::GetStyle();
                    const float fontScale  = ImGui::GetFontSize() / 17.0f;
                    const float sizeColumnWidth =
                        std::max(96.0f, 104.0f * fontScale);
                    const float typeColumnWidth =
                        std::max(76.0f, 86.0f * fontScale);
                    // 时间列按固定日期样本测量其首选宽度。
                    const float modifiedColumnPreferredWidth =
                        std::max(168.0f,
                                 ImGui::CalcTextSize("0000/00/00 00:00").x +
                                     tableStyle.CellPadding.x * 4.0f +
                                     ImGui::GetFrameHeight());
                    // 名称列必须容纳标题、padding 和一行控件高度。
                    const float nameColumnMinWidth =
                        std::max(148.0f,
                                 ImGui::CalcTextSize(
                                     TR("ui.file_manager.column_name").data())
                                         .x +
                                     tableStyle.CellPadding.x * 4.0f +
                                     ImGui::GetFrameHeight());
                    // 为滚动条和表格单元留出不参与数据列分配的宽度。
                    const float tableReserveWidth =
                        tableStyle.ScrollbarSize +
                        tableStyle.CellPadding.x * 2.0f;
                    // 名称列吸收扣除三列首选值后的剩余空间。
                    const float nameColumnWidth = std::max(
                        nameColumnMinWidth,
                        r.width - typeColumnWidth - sizeColumnWidth -
                            modifiedColumnPreferredWidth - tableReserveWidth);

                    if ( ImGui::BeginTable("FileTreeTableV2",
                                           4,
                                           tableFlags,
                                           { r.width, r.height }) ) {
                        // 表头在纵向滚动时保持可见。
                        ImGui::TableSetupScrollFreeze(0, 1);
                        // 前置数据列使用固定宽度，尾部修改时间列使用 Stretch
                        // 承接剩余空间，避免首列拖拽后在表格右侧留下死空白。
                        ImGui::TableSetupColumn(
                            TR("ui.file_manager.column_name").data(),
                            ImGuiTableColumnFlags_DefaultSort |
                                ImGuiTableColumnFlags_WidthFixed |
                                ImGuiTableColumnFlags_PreferSortAscending,
                            nameColumnWidth);
                        ImGui::TableSetupColumn(
                            TR("ui.file_manager.column_type").data(),
                            ImGuiTableColumnFlags_DefaultHide |
                                ImGuiTableColumnFlags_WidthFixed |
                                ImGuiTableColumnFlags_PreferSortAscending,
                            typeColumnWidth);
                        ImGui::TableSetupColumn(
                            TR("ui.file_manager.column_size").data(),
                            ImGuiTableColumnFlags_WidthFixed |
                                ImGuiTableColumnFlags_PreferSortAscending,
                            sizeColumnWidth);
                        ImGui::TableSetupColumn(
                            TR("ui.file_manager.column_modified_time").data(),
                            ImGuiTableColumnFlags_WidthStretch |
                                ImGuiTableColumnFlags_PreferSortDescending,
                            1.0f);
                        if ( ImGuiTable* table = ImGui::GetCurrentTable() ) {
                            // 使用项目自定义菜单替代 ImGui 默认列菜单。
                            table->DisableDefaultContextMenu = true;
                        }
                        ImGui::TableHeadersRow();
                        // 先消费表头产生的新排序规格，再绘制本帧行。
                        syncFileTableSortSpecs();

                        // 树层级缩进按两字符宽度适配当前字体。
                        const float indent = ImGui::CalcTextSize("AA").x;
                        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing,
                                            indent);
                        // 从项目根开始使用缓存快照递归展开目录。
                        drawDirectoryRecursive(m_currentRoot, sourceManager);
                        ImGui::PopStyleVar();

                        // 表头或列区域的右键菜单在表格结束前绘制。
                        renderFileSortContextMenu();
                        ImGui::EndTable();
                    }
                }
                // 空白区域右键菜单在表格窗口中但不绑定具体条目。
                renderFileBackgroundContextMenu(sourceManager);
            });
    }

    // 根容器统一应用小边距和 Header/表格间距。
    CLayVBox rootVBox;
    rootVBox.setPadding(rootPadding, rootPadding, rootPadding, rootPadding)
        .setSpacing(itemSpacing)
        .addLayout("treeVBox", treeVBox, Sizing::Grow(), Sizing::Grow());

    rootVBox.render(layoutContext);
}

/// @brief 按当前排序递归绘制一个目录快照及其展开子目录。
/// @param path 当前目录绝对路径。
/// @param sourceManager 用于激活谱面、音频或打开相关视图。
///
/// 每个条目占一行四列；第一列是透明背景 TreeNode，自行以整行 hover 色覆盖表格。
/// 只有目录节点处于展开状态时才递归获取子快照，折叠目录不会触发磁盘读取。
/// @warning UI 热路径：按可见展开树线性遍历缓存条目，不得每帧全量扫描项目根。
void FileManagerView::drawDirectoryRecursive(const std::filesystem::path& path,
                                             UIManager* sourceManager)
{
    // getDirectorySnapshot 命中缓存时只返回稳定引用。
    const auto& snapshot = getDirectorySnapshot(path);
    const float dpiScale =
        std::max(1.0f, Config::AppConfig::instance().getWindowContentScale());
    // 行高至少为 24 个缩放像素，并容纳当前 ImGui Frame。
    const float itemH = std::max(24.0f * dpiScale, ImGui::GetFrameHeight());

    for ( const auto& entry : snapshot.entries ) {
        // 固定最小行高让所有列和整行 hover 区域一致。
        ImGui::TableNextRow(ImGuiTableRowFlags_None, itemH);

        // 首列承载树层级、图标、名称和激活动作。
        ImGui::TableNextColumn();
        const std::string displayName =
            fmt::format("{}  {}", fileEntryIcon(entry), entry.filename);
        const float nameColumnWidth = ImGui::GetContentRegionAvail().x;
        // TreeNode 默认 header 颜色设透明，整行 hover 由表格背景统一处理。
        const ImVec4 transparent{ 0.0f, 0.0f, 0.0f, 0.0f };
        ImGui::PushStyleColor(ImGuiCol_Header, transparent);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, transparent);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, transparent);
        // 文件作为叶节点，目录返回 open 状态决定是否递归。
        const bool open = Utils::renderScrollingTreeNode(
            entry.fullPath,
            displayName,
            nameColumnWidth,
            itemH,
            !entry.isDirectory,
            [this, &entry, sourceManager]() {
                // 激活回调在当前条目引用仍有效的同一绘制调用内执行。
                activateFileEntry(entry, sourceManager);
            },
            "");
        ImGui::PopStyleColor(3);

        // 以 ImGui 表格行边界和首列结算高度判定 Hover，避免手算偏差。
        const ImGuiTable* table  = ImGui::GetCurrentTable();
        const float       mouseY = ImGui::GetMousePos().y;
        const float       rowMaxY =
            table ? std::max(table->RowPosY2,
                             ImGui::GetItemRectMax().y + table->RowCellPaddingY)
                  : 0.0f;
        // TableGetHoveredColumn 保证鼠标确实位于表格列区域。
        const bool rowHovered = table && ImGui::TableGetHoveredColumn() >= 0 &&
                                mouseY >= table->RowPosY1 && mouseY < rowMaxY;
        if ( rowHovered ) {
            // RowBg1 覆盖四列，形成完整文件管理器行反馈。
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                   ImGui::GetColorU32(ImGuiCol_HeaderHovered));
        }
        // 条目右键菜单与第一列 item 绑定，并允许行 hover 显示完整路径。
        renderFileEntryContextMenu(entry, sourceManager);
        if ( rowHovered ) {
            Utils::renderTooltip(entry.fullPath.c_str());
        }

        if ( ImGui::TableNextColumn() ) {
            // 隐藏类型列时 TableNextColumn 返回 false，跳过格式化和绘制。
            const std::string typeText = formatTypeColumn(entry);
            ImGui::TextUnformatted(typeText.c_str());
        }

        // 大小列由表格 API 自行跳过隐藏状态，文本构造保持轻量。
        ImGui::TableNextColumn();
        const std::string sizeText = formatSizeColumn(entry);
        ImGui::TextUnformatted(sizeText.c_str());

        // 修改时间在快照构建时已读取，此处只做本地格式化。
        ImGui::TableNextColumn();
        const std::string modifiedText = formatModifiedColumn(entry);
        ImGui::TextUnformatted(modifiedText.c_str());

        if ( entry.isDirectory && open ) {
            // 递归只发生在用户展开目录时，结束后必须对称 TreePop。
            drawDirectoryRecursive(entry.path, sourceManager);
            ImGui::TreePop();
        }
    }
}

/// @brief 获取或重建指定目录在当前排序配置下的缓存快照。
/// @param path 待读取目录。
/// @return 存储在 m_directoryCache 中的稳定快照引用。
///
/// 缓存键由词法规整 UTF-8 路径生成，并把排序字段、方向、目录优先作为命中条件。
/// 重建时逐条收集文件类型、大小、直属目录计数和修改时间，查询失败以有效位表达。
/// @warning 可能执行文件系统 IO；只应由可见目录缓存缺失或显式失效时调用。
const FileManagerView::DirectorySnapshot& FileManagerView::getDirectorySnapshot(
    const std::filesystem::path& path)
{
    // 排序配置完全一致时直接复用已排序条目。
    const auto cacheKey = directoryCacheKey(path);
    if ( const auto it = m_directoryCache.find(cacheKey);
         it != m_directoryCache.end() && it->second.sortKey == m_fileSortKey &&
         it->second.sortDirection == m_fileSortDirection &&
         it->second.directoriesFirst == m_directoriesFirst ) {
        return it->second;
    }

    // 新快照记录生成时的排序配置，供下次命中校验。
    DirectorySnapshot snapshot;
    snapshot.sortKey          = m_fileSortKey;
    snapshot.sortDirection    = m_fileSortDirection;
    snapshot.directoriesFirst = m_directoriesFirst;

    // error_code 遍历遵守禁异常约束并跳过权限不足目录。
    std::error_code filesystemError;
    constexpr auto  options =
        std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::directory_iterator iterator(
        path, options, filesystemError);
    const std::filesystem::directory_iterator endIterator;
    while ( !filesystemError && iterator != endIterator ) {
        // 路径对象在迭代器前进前复制进快照条目。
        const auto entryPath = iterator->path();
        if ( isInternalProjectStorageEntry(entryPath) ) {
            // 内部 .mmm 存储既不展示也不参与用户操作。
            iterator.increment(filesystemError);
            continue;
        }
        // 三种字符串表示分别服务于文件系统操作、可见名和稳定 ImGui ID。
        DirectoryEntryInfo info;
        info.path      = entryPath;
        info.filename  = Config::pathToUtf8(entryPath.filename());
        info.fullPath  = Config::pathToUtf8(entryPath);
        info.extension = Config::pathToUtf8(entryPath.extension());

        // 每个状态查询使用独立可清除 error_code，有错误时有效位保持 false。
        std::error_code entryError;
        info.isDirectory = iterator->is_directory(entryError) && !entryError;
        entryError.clear();
        info.isRegularFile =
            iterator->is_regular_file(entryError) && !entryError;

        if ( info.isDirectory ) {
            // 目录大小列展示排除内部存储后的直属条目数。
            if ( auto childCount = countDirectoryChildren(entryPath) ) {
                info.directoryChildCount    = *childCount;
                info.hasDirectoryChildCount = true;
            }
        } else if ( info.isRegularFile ) {
            // 非普通文件不查询字节大小。
            entryError.clear();
            const auto fileSize = iterator->file_size(entryError);
            if ( !entryError ) {
                info.fileSize    = fileSize;
                info.hasFileSize = true;
            }
        }

        entryError.clear();
        // 文件和目录都尝试获取最后修改时间。
        const auto modifiedTime = iterator->last_write_time(entryError);
        if ( !entryError ) {
            info.lastWriteTime    = modifiedTime;
            info.hasLastWriteTime = true;
        }

        // 完整值对象移入快照，之后不再依赖 directory_entry 生命周期。
        snapshot.entries.push_back(std::move(info));
        iterator.increment(filesystemError);
    }

    // 即使遍历中途失败，也把已收集条目按确定规则排序供 UI 展示。
    std::stable_sort(
        snapshot.entries.begin(),
        snapshot.entries.end(),
        [this](const DirectoryEntryInfo& lhs, const DirectoryEntryInfo& rhs) {
            return fileEntryLess(lhs,
                                 rhs,
                                 m_fileSortKey,
                                 m_fileSortDirection,
                                 m_directoriesFirst);
        });

    // insert_or_assign 同时处理首次缓存和排序配置变化后的替换。
    auto [it, inserted] =
        m_directoryCache.insert_or_assign(cacheKey, std::move(snapshot));
    return it->second;
}

/// @brief 使所有目录快照失效。
///
/// 文件操作、排序变化和显式刷新共用该入口；clear 不同步扫描文件系统，下一帧只按
/// 当前展开目录惰性重建。
void FileManagerView::invalidateDirectoryCache()
{
    // 保留容器自身容量策略，避免主动 shrink 带来额外分配。
    m_directoryCache.clear();
}

/// @brief 合并后台文件监视器提交的目录刷新请求。
///
/// 队列元素只表示至少需要刷新一次，本帧排空全部请求后统一清缓存，避免短时间多个
/// 文件事件导致重复重建。
/// @warning UI 热路径：每帧调用非阻塞 try_dequeue，不等待生产线程。
void FileManagerView::consumePendingDirectoryRefreshes()
{
    // 输出值内容不参与语义，只需记录是否成功取到任一元素。
    bool hasRefreshRequest = false;
    bool refreshRequest    = false;
    while ( m_pendingDirectoryRefreshes.try_dequeue(refreshRequest) ) {
        // 多个通知折叠为一个失效标志。
        hasRefreshRequest = true;
    }

    if ( hasRefreshRequest ) {
        // 重建延迟到实际绘制展开目录时发生。
        invalidateDirectoryCache();
    }
}

/// @brief 把 ImGui 表头排序规格同步到文件树缓存配置。
///
/// 当前只使用第一排序列；无规格或 SpecsDirty 为 false
/// 时保持现状。列索引映射到内部
/// FileSortKey，方向映射到二态枚举，真正变化时才使目录缓存失效。
/// @warning UI 热路径：每帧表头后调用，只在用户改变排序时清缓存。
void FileManagerView::syncFileTableSortSpecs()
{
    // ImGui 以 dirty 标志要求应用层消费新规格。
    ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
    if ( !sortSpecs || sortSpecs->SpecsCount <= 0 || !sortSpecs->SpecsDirty ) {
        return;
    }

    // 未识别列安全回退名称排序。
    const ImGuiTableColumnSortSpecs& primarySpec = sortSpecs->Specs[0];
    FileSortKey                      newSortKey  = FileSortKey::Name;
    if ( primarySpec.ColumnIndex == FileTableColumnType ) {
        // 枚举值与表格 setup 顺序保持一致。
        newSortKey = FileSortKey::Type;
    } else if ( primarySpec.ColumnIndex == FileTableColumnSize ) {
        newSortKey = FileSortKey::Size;
    } else if ( primarySpec.ColumnIndex == FileTableColumnModifiedTime ) {
        newSortKey = FileSortKey::ModifiedTime;
    }

    // ImGui 非 Descending 值统一按升序处理。
    const SortDirection newDirection =
        primarySpec.SortDirection == ImGuiSortDirection_Descending
            ? SortDirection::Descending
            : SortDirection::Ascending;

    if ( newSortKey != m_fileSortKey || newDirection != m_fileSortDirection ) {
        // 配置写入与缓存失效在同一 UI 帧完成。
        m_fileSortKey       = newSortKey;
        m_fileSortDirection = newDirection;
        invalidateDirectoryCache();
    }
    // 无论配置是否变化都确认已消费本次 ImGui 请求。
    sortSpecs->SpecsDirty = false;
}

/// @brief 绘制文件表格列宽、排序、刷新和列显隐上下文菜单。
///
/// 菜单复用当前 ImGuiTable
/// 内部状态：可自动适配命中列或全部列，选择任一字段升降序，
/// 切换目录优先，并排队修改非名称列的下一帧可见性。名称列始终保留导航入口。
/// @warning UI 热路径：表格绘制期间调用，菜单未打开时只做常量状态查询。
void FileManagerView::renderFileSortContextMenu()
{
    // 必须位于 BeginTable 与 EndTable 之间才能访问当前表。
    ImGuiTable* table = ImGui::GetCurrentTable();
    if ( !table ) {
        return;
    }

    // 提高最小 padding，避免紧凑主题下菜单项过于拥挤。
    ImGuiStyle&  style = ImGui::GetStyle();
    const ImVec2 popupPadding(std::max(style.WindowPadding.x, 8.0f),
                              std::max(style.WindowPadding.y, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popupPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(std::max(style.ItemSpacing.x, 8.0f),
                               std::max(style.ItemSpacing.y, 4.0f)));
    // 样式压栈必须在弹窗未打开的早退分支对称恢复。
    const bool popupOpen = ImGui::TableBeginContextMenuPopup(table);
    if ( !popupOpen ) {
        ImGui::PopStyleVar(2);
        return;
    }

    // ContextPopupColumn 只有在有效列区域右键时才可用于单列适配。
    const int contextColumn =
        table->ContextPopupColumn >= 0 &&
                table->ContextPopupColumn < table->ColumnsCount
            ? table->ContextPopupColumn
            : -1;
    if ( contextColumn >= 0 && isTableColumnEnabled(table, contextColumn) &&
         ::MMM::UI::FeedbackMenuItem(
             TR("ui.resource_table.size_column_fit").data()) ) {
        ImGui::TableSetColumnWidthAutoSingle(table, contextColumn);
    }

    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.resource_table.size_all_default").data()) ) {
        ImGui::TableSetColumnWidthAutoAll(table);
    }

    // 排序 helper 只在状态实际变化时失效目录缓存。
    auto applySort = [&](FileSortKey sortKey, SortDirection direction) {
        if ( m_fileSortKey != sortKey || m_fileSortDirection != direction ) {
            m_fileSortKey       = sortKey;
            m_fileSortDirection = direction;
            invalidateDirectoryCache();
        }
    };
    // 菜单项 helper 组合本地化方向后缀并展示当前选中态。
    auto sortMenuItem = [&](FileSortKey   sortKey,
                            SortDirection direction,
                            const char*   columnLabel) {
        const std::string label = makeSortMenuLabel(columnLabel, direction);
        const bool        selected =
            m_fileSortKey == sortKey && m_fileSortDirection == direction;
        if ( ::MMM::UI::FeedbackMenuItem(label.c_str(), nullptr, selected) ) {
            applySort(sortKey, direction);
        }
    };

    if ( ::MMM::UI::FeedbackBeginMenu(TR("ui.resource_table.sort").data()) ) {
        // 四个字段分别提供升序和降序两个明确入口。
        sortMenuItem(FileSortKey::Name,
                     SortDirection::Ascending,
                     TR("ui.file_manager.column_name").data());
        sortMenuItem(FileSortKey::Name,
                     SortDirection::Descending,
                     TR("ui.file_manager.column_name").data());
        sortMenuItem(FileSortKey::Type,
                     SortDirection::Ascending,
                     TR("ui.file_manager.column_type").data());
        sortMenuItem(FileSortKey::Type,
                     SortDirection::Descending,
                     TR("ui.file_manager.column_type").data());
        sortMenuItem(FileSortKey::Size,
                     SortDirection::Ascending,
                     TR("ui.file_manager.column_size").data());
        sortMenuItem(FileSortKey::Size,
                     SortDirection::Descending,
                     TR("ui.file_manager.column_size").data());
        sortMenuItem(FileSortKey::ModifiedTime,
                     SortDirection::Ascending,
                     TR("ui.file_manager.column_modified_time").data());
        sortMenuItem(FileSortKey::ModifiedTime,
                     SortDirection::Descending,
                     TR("ui.file_manager.column_modified_time").data());
        ::MMM::UI::FeedbackEndMenu();
    }

    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.refresh").data()) ) {
        // 显式刷新只标记失效，展开目录在随后绘制中惰性扫描。
        invalidateDirectoryCache();
    }

    bool directoriesFirst = m_directoriesFirst;
    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.directories_first").data(),
             nullptr,
             directoriesFirst) ) {
        m_directoriesFirst = !m_directoriesFirst;
        invalidateDirectoryCache();
    }

    if ( ::MMM::UI::FeedbackBeginMenu(TR("ui.resource_table.reset").data()) ) {
        if ( ::MMM::UI::FeedbackMenuItem(
                 TR("ui.resource_table.reset_all").data()) ) {
            ImGui::TableResetSettings(table);
            applySort(FileSortKey::Name, SortDirection::Ascending);
        }
        if ( ::MMM::UI::FeedbackMenuItem(
                 TR("ui.resource_table.reset_columns").data()) ) {
            ImGui::TableSetColumnWidthAutoAll(table);
        }
        if ( ::MMM::UI::FeedbackMenuItem(
                 TR("ui.resource_table.show_all_columns").data()) ) {
            for ( int column = 0; column < table->ColumnsCount; ++column ) {
                queueTableColumnEnabled(table, column, true);
            }
        }
        if ( ::MMM::UI::FeedbackMenuItem(
                 TR("ui.resource_table.reset_sort").data()) ) {
            applySort(FileSortKey::Name, SortDirection::Ascending);
        }
        ::MMM::UI::FeedbackEndMenu();
    }

    ImGui::Separator();

    const std::array<const char*, 4> columnLabels{
        TR("ui.file_manager.column_name").data(),
        TR("ui.file_manager.column_type").data(),
        TR("ui.file_manager.column_size").data(),
        TR("ui.file_manager.column_modified_time").data()
    };
    int enabledColumnCount = 0;
    for ( int column = 0; column < table->ColumnsCount; ++column ) {
        if ( isTableColumnUserEnabled(table, column) ) {
            enabledColumnCount++;
        }
    }
    for ( int column = 0; column < table->ColumnsCount; ++column ) {
        const bool enabled   = isTableColumnUserEnabled(table, column);
        const bool canToggle = !enabled || enabledColumnCount > 1;
        if ( ::MMM::UI::FeedbackMenuItem(
                 columnLabels[column], nullptr, enabled, canToggle) ) {
            queueTableColumnEnabled(table, column, !enabled);
        }
    }

    ImGui::EndPopup();
    ImGui::PopStyleVar(2);
}

/// @brief 绘制文件树空白区域的项目级上下文菜单。
/// @param sourceManager 用于打开新谱面向导。
///
/// NoOpenOverItems
/// 确保条目右键由条目菜单处理。空白菜单以项目根为新建和粘贴目标，
/// 并提供显式缓存刷新；剪贴板无效时保持粘贴项可见但禁用。
void FileManagerView::renderFileBackgroundContextMenu(UIManager* sourceManager)
{
    // 右键只在没有条目 item 命中的窗口区域打开。
    constexpr ImGuiPopupFlags popupFlags =
        ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems;
    if ( !ImGui::BeginPopupContextWindow("FileTreeBackgroundContextMenu",
                                         popupFlags) ) {
        return;
    }

    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.new_beatmap").data()) ) {
        // 新谱面属于当前项目，与具体目录条目无关。
        openNewBeatmapWizard(sourceManager);
    }
    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.new_folder").data()) ) {
        // 空白区新文件夹固定创建在项目根。
        requestNewFolder(m_currentRoot);
    }

    // 每帧重新验证剪贴板源仍存在且位于当前项目。
    const bool canPaste = hasPasteableFileClipboard();
    if ( ::MMM::UI::FeedbackMenuItem(TR("ui.file_manager.context.paste").data(),
                                     nullptr,
                                     false,
                                     canPaste) ) {
        // 目标为项目根，实际操作还会再次校验目录边界。
        pasteFileClipboardInto(m_currentRoot);
    }

    ImGui::Separator();
    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.refresh").data()) ) {
        invalidateDirectoryCache();
    }

    ImGui::EndPopup();
}

/// @brief 绘制单个文件或目录条目的操作菜单。
/// @param entry 当前快照中的目标条目。
/// @param sourceManager 用于激活条目或打开向导。
///
/// popup ID
/// 包含完整路径，避免同名文件冲突。文件的新建和粘贴目标是其父目录，目录
/// 则以自身为目标；重命名、剪切、复制和删除均只记录请求，确认操作在独立弹窗执行。
void FileManagerView::renderFileEntryContextMenu(
    const DirectoryEntryInfo& entry, UIManager* sourceManager)
{
    // 完整路径构造稳定且唯一的上下文菜单 ID。
    const std::string popupId = "FileEntryContext_" + entry.fullPath;
    if ( !ImGui::BeginPopupContextItem(popupId.c_str(),
                                       ImGuiPopupFlags_MouseButtonRight) ) {
        return;
    }

    // 文件操作落在父目录，目录操作落在目录内部。
    const auto targetDirectory =
        entry.isDirectory ? entry.path : entry.path.parent_path();

    if ( !entry.isDirectory && (isBeatmapExtension(entry.extension) ||
                                isAudioExtension(entry.extension)) ) {
        // 只有已知可激活文件类型显示打开入口。
        if ( ::MMM::UI::FeedbackMenuItem(
                 TR("ui.file_manager.context.open").data()) ) {
            activateFileEntry(entry, sourceManager);
        }
    }

    // 新谱面仍由项目级向导创建，不依赖条目类型。
    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.new_beatmap").data()) ) {
        openNewBeatmapWizard(sourceManager);
    }
    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.new_folder").data()) ) {
        requestNewFolder(targetDirectory);
    }

    // 粘贴可用性在菜单绘制时即时验证。
    const bool canPaste = hasPasteableFileClipboard();
    if ( ::MMM::UI::FeedbackMenuItem(TR("ui.file_manager.context.paste").data(),
                                     nullptr,
                                     false,
                                     canPaste) ) {
        pasteFileClipboardInto(targetDirectory);
    }

    // 以下四项绑定当前条目本身。
    ImGui::Separator();
    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.rename").data()) ) {
        requestRename(entry.path);
    }
    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.cut").data()) ) {
        setFileClipboard(entry.path, true);
    }
    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.copy").data()) ) {
        setFileClipboard(entry.path, false);
    }
    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.delete").data()) ) {
        requestDelete(entry.path);
    }

    ImGui::Separator();
    if ( entry.isDirectory &&
         ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.refresh").data()) ) {
        // 目录刷新只移除该节点快照，其子快照可继续按键复用。
        m_directoryCache.erase(directoryCacheKey(entry.path));
    }

    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.open_path").data()) ) {
        // 文件要求系统管理器选中条目，目录直接打开自身。
        if ( !DesktopPathUtils::openInFileManager(entry.path,
                                                  !entry.isDirectory) ) {
            XERROR("无法在系统文件管理器中打开路径：{}", entry.fullPath);
        }
    }

    ImGui::EndPopup();
}

/// @brief 从 UIManager 查找并打开已注册的新谱面向导。
/// @param sourceManager 当前 UI 管理器；为空时安全忽略。
///
/// FileManager 不创建向导，保持视图所有权和唯一实例由 UIManager 统一管理。
void FileManagerView::openNewBeatmapWizard(UIManager* sourceManager)
{
    if ( !sourceManager ) {
        // 生命周期过渡中可能没有可用管理器。
        return;
    }

    auto* wizard = sourceManager->getView<NewBeatmapWizard>("NewBeatmapWizard");
    if ( wizard ) {
        // open 自行重置上一次创建流程状态。
        wizard->open();
    }
}

/// @brief 将 UTF-8 初始值安全写入文件操作固定输入缓冲。
/// @param value 重命名原文件名或新文件夹默认名。
///
/// 缓冲先整体清零，再最多复制容量减一的字节并显式终止，供 ImGui::InputText
/// 使用。
void FileManagerView::setFileOperationInput(const std::string& value)
{
    // 清除旧输入尾部，避免较短新值后残留字符。
    m_fileOperationInput.fill('\0');
    // 为 NUL 终止符始终保留一个位置。
    const auto copySize =
        std::min(m_fileOperationInput.size() - 1, value.size());
    std::memcpy(m_fileOperationInput.data(), value.data(), copySize);
    m_fileOperationInput[copySize] = '\0';
}

/// @brief 准备指定条目的重命名确认弹窗。
/// @param path 当前条目的路径。
///
/// 请求阶段不访问文件系统，只保存目标、预填
/// basename、清除旧错误并请求首帧聚焦。
void FileManagerView::requestRename(const std::filesystem::path& path)
{
    // 完整路径保留到用户确认时重新验证存在性和项目边界。
    m_pendingRenamePath = path;
    m_fileOperationError.clear();
    setFileOperationInput(Config::pathToUtf8(path.filename()));
    // 弹窗打开和键盘焦点均由后续 renderFileOperationPopups 消费一次。
    m_shouldOpenRenamePopup         = true;
    m_shouldFocusFileOperationInput = true;
}

/// @brief 准备在指定目录创建文件夹的输入弹窗。
/// @param directory 期望的父目录。
///
/// 默认名称来自翻译配置，确认时仍会重新校验父目录存在且位于项目根内。
void FileManagerView::requestNewFolder(const std::filesystem::path& directory)
{
    // 父目录保存到确认阶段，允许弹窗跨帧存在。
    m_pendingNewFolderDirectory = directory;
    m_fileOperationError.clear();
    setFileOperationInput(TR("ui.file_manager.default_new_folder_name").data());
    m_shouldOpenNewFolderPopup      = true;
    m_shouldFocusFileOperationInput = true;
}

/// @brief 准备指定条目的删除确认弹窗。
/// @param path 待删除文件或目录。
///
/// 删除动作不会在右键菜单中立即执行，避免误触和菜单遍历期间改变目录快照。
void FileManagerView::requestDelete(const std::filesystem::path& path)
{
    // 确认阶段重新检查路径、项目边界和音频资源约束。
    m_pendingDeletePath = path;
    m_fileOperationError.clear();
    m_shouldOpenDeletePopup = true;
}

/// @brief 设置文件管理器内部的剪切或复制来源。
/// @param path 源条目路径。
/// @param cut true 表示移动，false 表示复制。
///
/// 剪贴板只接受仍存在且位于当前项目根内的路径；验证失败会清空旧状态，防止跨项目
/// 或陈旧路径在后续粘贴时被使用。
void FileManagerView::setFileClipboard(const std::filesystem::path& path,
                                       bool                         cut)
{
    if ( !filesystemPathExists(path) ||
         !pathIsInsideOrSame(m_currentRoot, path) ) {
        // 无效新来源同时废弃之前的剪贴板状态。
        m_fileClipboardPath.clear();
        m_fileClipboardMode = FileClipboardMode::None;
        return;
    }

    // 路径和值枚举共同构成有效剪贴板不变量。
    m_fileClipboardPath = path;
    m_fileClipboardMode =
        cut ? FileClipboardMode::Cut : FileClipboardMode::Copy;
}

/// @brief 重新验证内部文件剪贴板能否粘贴到当前项目。
/// @return 模式有效、源存在且仍位于项目根内时返回 true。
///
/// 每次菜单绘制和执行前都调用，覆盖文件被外部删除或当前项目根变化的情况。
bool FileManagerView::hasPasteableFileClipboard() const
{
    return m_fileClipboardMode != FileClipboardMode::None &&
           filesystemPathExists(m_fileClipboardPath) &&
           pathIsInsideOrSame(m_currentRoot, m_fileClipboardPath);
}

/// @brief 将内部剪贴板条目复制或移动到目标目录。
/// @param targetDirectory 用户右键选择的目标目录。
///
/// 操作前重新验证源、目标和项目边界，并禁止目录粘贴进自身子树。目标名称始终避让
/// 既有条目。剪切先尝试原子 rename，跨文件系统失败时回退递归复制后删除源；涉及
/// 受管音频时在移动前验证，在成功后同步项目资源路径。
/// @warning 低频用户操作：可能递归复制或删除目录，不得从每帧更新路径调用。
void FileManagerView::pasteFileClipboardInto(
    const std::filesystem::path& targetDirectory)
{
    // 新操作开始时清除上一次错误提示。
    m_fileOperationError.clear();
    if ( !hasPasteableFileClipboard() ) {
        // 源模式、存在性或项目归属任一失效都拒绝操作。
        m_fileOperationError =
            TR("ui.file_manager.error_clipboard_empty").data();
        return;
    }
    if ( !filesystemPathIsDirectory(targetDirectory) ||
         !pathIsInsideOrSame(m_currentRoot, targetDirectory) ) {
        // 目标必须是当前项目内仍存在的目录。
        m_fileOperationError =
            TR("ui.file_manager.error_target_not_directory").data();
        return;
    }
    if ( filesystemPathIsDirectory(m_fileClipboardPath) &&
         pathIsInsideOrSame(m_fileClipboardPath, targetDirectory) ) {
        // 禁止递归把目录复制进自身或任意后代。
        m_fileOperationError =
            TR("ui.file_manager.error_paste_into_self").data();
        return;
    }

    // 唯一目标避免复制或移动覆盖任何已有用户数据。
    const auto destination =
        makeUniqueDestinationPath(m_fileClipboardPath, targetDirectory);
    std::error_code filesystemError;
    if ( m_fileClipboardMode == FileClipboardMode::Cut ) {
        // 受管音频移动必须先由逻辑层确认没有不可更新引用。
        m_fileOperationError =
            Logic::EditorEngine::instance().validateAudioResourceMove(
                m_fileClipboardPath, destination);
        if ( !m_fileOperationError.empty() ) return;

        // 保存源路径供成功移动后的项目资源重映射使用。
        const auto movedSourcePath = m_fileClipboardPath;
        // 同文件系统 rename 优先，通常是原子且无需复制数据。
        std::filesystem::rename(
            m_fileClipboardPath, destination, filesystemError);
        if ( filesystemError ) {
            // rename 失败可能是跨文件系统，清错后尝试复制删除回退。
            filesystemError.clear();
            if ( copyFilesystemEntry(
                     m_fileClipboardPath, destination, filesystemError) ) {
                std::error_code removeError;
                if ( filesystemPathIsDirectory(m_fileClipboardPath) ) {
                    // 目录复制成功后递归删除原树。
                    std::filesystem::remove_all(m_fileClipboardPath,
                                                removeError);
                } else {
                    // 普通文件只删除单个源条目。
                    std::filesystem::remove(m_fileClipboardPath, removeError);
                }
                // 删除失败仍视为移动失败，避免报告成功但留下两份。
                filesystemError = removeError;
            }
        }
        if ( filesystemError ) {
            // 错误保留剪贴板，允许用户修正目标后重试。
            m_fileOperationError = formatFilesystemError(
                TR("ui.file_manager.operation_paste").data(), filesystemError);
            XERROR("Failed to move file manager clipboard from {} to {}: {}",
                   Config::pathToUtf8(m_fileClipboardPath),
                   Config::pathToUtf8(destination),
                   filesystemError.message());
            return;
        }
        // 文件系统成功后再更新项目音频路径及谱面引用。
        m_fileOperationError =
            syncMovedProjectAudioResourcePaths(movedSourcePath, destination);
        if ( !m_fileOperationError.empty() ) return;
        // 完整移动成功后剪切剪贴板只消费一次。
        m_fileClipboardPath.clear();
        m_fileClipboardMode = FileClipboardMode::None;
    } else {
        // 复制模式保留源和剪贴板，可继续粘贴到其他目录。
        if ( !copyFilesystemEntry(
                 m_fileClipboardPath, destination, filesystemError) ) {
            // 目标唯一化和不覆盖策略保护既有文件。
            m_fileOperationError = formatFilesystemError(
                TR("ui.file_manager.operation_paste").data(), filesystemError);
            XERROR("Failed to copy file manager clipboard from {} to {}: {}",
                   Config::pathToUtf8(m_fileClipboardPath),
                   Config::pathToUtf8(destination),
                   filesystemError.message());
            return;
        }
    }

    // 成功后让展开目录在下一帧反映新结构。
    invalidateDirectoryCache();
}

/// @brief 校验输入并执行待处理条目的重命名。
///
/// 输入必须是单一文件名；源和目标父目录均需位于当前项目，目标不得已存在。受管
/// 音频先经逻辑层验证，文件系统 rename
/// 成功后再同步资源引用，最后失效缓存并关窗。
/// @warning 低频用户确认路径：执行一次文件系统 rename 和可能的项目保存。
void FileManagerView::confirmRename()
{
    // 每次确认重新计算错误，不沿用上一次尝试。
    m_fileOperationError.clear();
    const std::string newName = m_fileOperationInput.data();
    if ( !isValidSingleFilename(newName) ) {
        // 拒绝空值、点目录和包含路径分隔符的输入。
        m_fileOperationError = TR("ui.file_manager.error_invalid_name").data();
        return;
    }
    if ( !filesystemPathExists(m_pendingRenamePath) ||
         !pathIsInsideOrSame(m_currentRoot, m_pendingRenamePath) ) {
        // 弹窗期间源可能被外部移动、删除或切换到其他项目。
        m_fileOperationError =
            TR("ui.file_manager.error_missing_source").data();
        return;
    }

    // 目标固定在原父目录，输入无法改变层级。
    const auto destination =
        m_pendingRenamePath.parent_path() / Config::utf8ToPath(newName);
    if ( destination == m_pendingRenamePath ) {
        // 未改变名称视为成功取消，不执行无意义文件操作。
        ImGui::CloseCurrentPopup();
        return;
    }
    if ( filesystemPathExists(destination) ) {
        // 绝不覆盖同名目标。
        m_fileOperationError = TR("ui.file_manager.error_target_exists").data();
        return;
    }
    if ( !pathIsInsideOrSame(m_currentRoot, destination.parent_path()) ) {
        // 防御符号链接或陈旧路径导致目标父级越界。
        m_fileOperationError =
            TR("ui.file_manager.error_outside_project").data();
        return;
    }

    // 音频路径引用无法安全改写时在触碰磁盘前停止。
    m_fileOperationError =
        Logic::EditorEngine::instance().validateAudioResourceMove(
            m_pendingRenamePath, destination);
    if ( !m_fileOperationError.empty() ) return;

    // error_code 重载避免文件系统异常穿过 UI 边界。
    std::error_code filesystemError;
    std::filesystem::rename(m_pendingRenamePath, destination, filesystemError);
    if ( filesystemError ) {
        // 底层消息同时写 UI 提示和结构化日志。
        m_fileOperationError = formatFilesystemError(
            TR("ui.file_manager.operation_rename").data(), filesystemError);
        XERROR("Failed to rename {} to {}: {}",
               Config::pathToUtf8(m_pendingRenamePath),
               Config::pathToUtf8(destination),
               filesystemError.message());
        return;
    }

    // 重命名成功后同步受管音频的项目相对路径。
    m_fileOperationError =
        syncMovedProjectAudioResourcePaths(m_pendingRenamePath, destination);
    if ( !m_fileOperationError.empty() ) return;
    // 所有状态更新成功后刷新树并关闭确认弹窗。
    invalidateDirectoryCache();
    ImGui::CloseCurrentPopup();
}

/// @brief 校验输入并在待处理父目录下创建一个直属文件夹。
///
/// 文件名规则与重命名一致；父目录必须仍存在且位于当前项目，目标冲突时不覆盖。
/// 只创建单层目录，不隐式创建缺失父级。
/// @warning 低频用户确认路径：只执行一次 create_directory。
void FileManagerView::confirmNewFolder()
{
    // 新尝试先清除旧错误。
    m_fileOperationError.clear();
    const std::string folderName = m_fileOperationInput.data();
    if ( !isValidSingleFilename(folderName) ) {
        // 输入只能表示一个直属目录名称。
        m_fileOperationError = TR("ui.file_manager.error_invalid_name").data();
        return;
    }
    if ( !filesystemPathIsDirectory(m_pendingNewFolderDirectory) ||
         !pathIsInsideOrSame(m_currentRoot, m_pendingNewFolderDirectory) ) {
        // 弹窗期间父目录可能已被删除或移出项目。
        m_fileOperationError =
            TR("ui.file_manager.error_target_not_directory").data();
        return;
    }

    // 使用路径拼接而非字符串连接，保持平台分隔符正确。
    const auto newFolderPath =
        m_pendingNewFolderDirectory / Config::utf8ToPath(folderName);
    if ( filesystemPathExists(newFolderPath) ) {
        // 不复用或清空已存在目录，避免意外写入。
        m_fileOperationError = TR("ui.file_manager.error_target_exists").data();
        return;
    }

    // error_code 接收权限、保留名和其他平台错误。
    std::error_code filesystemError;
    std::filesystem::create_directory(newFolderPath, filesystemError);
    if ( filesystemError ) {
        // 失败保留弹窗和输入，允许用户修改后重试。
        m_fileOperationError = formatFilesystemError(
            TR("ui.file_manager.operation_new_folder").data(), filesystemError);
        XERROR("Failed to create folder {}: {}",
               Config::pathToUtf8(newFolderPath),
               filesystemError.message());
        return;
    }

    // 成功后刷新树并结束模态操作。
    invalidateDirectoryCache();
    ImGui::CloseCurrentPopup();
}

/// @brief 校验并删除待处理文件或目录。
///
/// 项目根永远不可删除。目录若包含任何音频扩展文件会保守拒绝；单个音频文件必须
/// 是项目已登记资源，并通过 CmdRemoveAudioResource 让逻辑层处理引用与磁盘删除。
/// 其他文件和无音频目录使用 error_code 文件系统 API，成功后清理受影响剪贴板。
/// @warning 低频用户确认路径：目录检查和 remove_all 可能递归访问文件系统。
void FileManagerView::confirmDelete()
{
    // 新确认尝试清除前一次错误。
    m_fileOperationError.clear();
    if ( !filesystemPathExists(m_pendingDeletePath) ||
         !pathIsInsideOrSame(m_currentRoot, m_pendingDeletePath) ||
         comparableFilesystemPath(m_pendingDeletePath) ==
             comparableFilesystemPath(m_currentRoot) ) {
        // 源必须存在、属于项目且不能等于项目根。
        m_fileOperationError =
            TR("ui.file_manager.error_missing_source").data();
        return;
    }

    std::error_code filesystemError;
    if ( filesystemPathIsDirectory(m_pendingDeletePath) ) {
        // 目录删除前扫描潜在受管音频，避免绕过资源解绑逻辑。
        const auto containsAudio =
            directoryContainsAudioFile(m_pendingDeletePath, filesystemError);
        if ( !containsAudio ) {
            // 遍历未知时保守停止，不把目录误判为安全。
            m_fileOperationError = formatFilesystemError(
                TR("ui.file_manager.operation_delete").data(), filesystemError);
            return;
        }
        if ( *containsAudio ) {
            // 用户需先通过音频管理流程移除目录内资源。
            m_fileOperationError =
                TR("ui.file_manager.error_directory_contains_audio").data();
            return;
        }
        // 已确认无音频后才允许递归删除目录树。
        std::filesystem::remove_all(m_pendingDeletePath, filesystemError);
    } else {
        // 普通文件先按扩展名识别是否需要逻辑资源命令。
        const auto extension =
            Config::pathToUtf8(m_pendingDeletePath.extension());
        if ( isAudioExtension(extension) ) {
            // 精确路径查找避免删除未登记或同名的错误音轨。
            auto&       engine   = Logic::EditorEngine::instance();
            auto*       project  = engine.getCurrentProject();
            const auto* resource = project ? findProjectAudioResourceAtPath(
                                                 *project, m_pendingDeletePath)
                                           : nullptr;
            if ( !resource ) {
                // 未登记音频不能直接从文件树删除，防止资源状态不一致。
                m_fileOperationError =
                    TR("ui.file_manager.error_audio_not_registered").data();
                return;
            }

            // 复制稳定 ID，避免异步命令持有项目资源元素地址。
            const auto resourceId = resource->m_id;
            // true 请求逻辑命令同时删除底层文件。
            engine.pushCommand(
                Logic::CmdRemoveAudioResource{ resourceId, true });
            if ( pathIsInsideOrSame(m_pendingDeletePath,
                                    m_fileClipboardPath) ) {
                // 删除源或其后代会使内部剪贴板失效。
                m_fileClipboardPath.clear();
                m_fileClipboardMode = FileClipboardMode::None;
            }
            // 命令异步执行，先失效缓存并关闭确认弹窗。
            invalidateDirectoryCache();
            ImGui::CloseCurrentPopup();
            return;
        }
        // 非音频普通文件可直接删除单个条目。
        std::filesystem::remove(m_pendingDeletePath, filesystemError);
    }
    if ( filesystemError ) {
        // 文件系统失败保留弹窗，展示系统原因并写日志。
        m_fileOperationError = formatFilesystemError(
            TR("ui.file_manager.operation_delete").data(), filesystemError);
        XERROR("Failed to delete {}: {}",
               Config::pathToUtf8(m_pendingDeletePath),
               filesystemError.message());
        return;
    }

    if ( pathIsInsideOrSame(m_pendingDeletePath, m_fileClipboardPath) ) {
        // 目录删除也可能包含当前剪贴板源。
        m_fileClipboardPath.clear();
        m_fileClipboardMode = FileClipboardMode::None;
    }
    // 成功完成同步删除后刷新树并关闭弹窗。
    invalidateDirectoryCache();
    ImGui::CloseCurrentPopup();
}

/// @brief 驱动重命名、新建文件夹和删除三个确认弹窗。
/// @param dpiScale 当前窗口内容缩放。
///
/// 名称类弹窗共享输入、错误、Enter 提交和按钮布局 helper；各 popup 标题使用 ###
/// 固定内部 ID。打开请求和首次聚焦均为一次性状态，确认函数负责成功时关闭
/// popup。
/// @warning UI 热路径：每帧调用，弹窗未打开时只检查少量布尔状态。
void FileManagerView::renderFileOperationPopups(float dpiScale)
{
    // 泛型 lambda 复用重命名和新目录的相同交互外壳。
    auto renderNamePopup = [&](const char* title,
                               const char* label,
                               const char* confirmLabel,
                               auto&&      onConfirm) {
        // open 只服务当前 Begin 调用，持久状态由请求布尔位维护。
        bool                           open = true;
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin(title,
                              &open,
                              ImGuiWindowFlags_NoCollapse,
                              { 360.0f * dpiScale, 0.0f }) ) {
            ImGui::TextUnformatted(label);
            if ( m_shouldFocusFileOperationInput ) {
                // 只在弹窗首次出现时把键盘焦点交给输入框。
                ImGui::SetKeyboardFocusHere();
                m_shouldFocusFileOperationInput = false;
            }
            // EnterReturnsTrue 与确认按钮共用同一回调。
            const bool enterPressed =
                ImGui::InputText("##FileManagerOperationName",
                                 m_fileOperationInput.data(),
                                 m_fileOperationInput.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
            if ( !m_fileOperationError.empty() ) {
                // 失败保留弹窗和输入，方便用户直接修正。
                ImGui::TextDisabled("%s", m_fileOperationError.c_str());
            }
            const ImVec2 buttonSize{ 128.0f * dpiScale, 0.0f };
            const bool   confirmClicked =
                ::MMM::UI::FeedbackButton(confirmLabel, buttonSize);
            if ( enterPressed || confirmClicked ) {
                // confirmRename 或 confirmNewFolder 重新执行全部边界校验。
                onConfirm();
            }
            ImGui::SameLine();
            // 取消只关闭当前 popup，不执行文件操作。
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           buttonSize) ) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    };

    // 翻译文本作为可见标题，稳定后缀保存 popup 身份。
    const std::string renameTitle =
        fmt::format("{}###FileManagerRenamePopup",
                    TR("ui.file_manager.rename_title").data());
    if ( m_shouldOpenRenamePopup ) {
        // 消费一次打开请求，避免关闭后被每帧重开。
        ::MMM::UI::FeedbackOpenPopup(renameTitle.c_str());
        m_shouldOpenRenamePopup = false;
    }
    renderNamePopup(renameTitle.c_str(),
                    TR("ui.file_manager.rename_label").data(),
                    TR("ui.file_manager.context.rename").data(),
                    [&]() { confirmRename(); });

    // 新目录弹窗复用相同名称输入 helper。
    const std::string newFolderTitle =
        fmt::format("{}###FileManagerNewFolderPopup",
                    TR("ui.file_manager.new_folder_title").data());
    if ( m_shouldOpenNewFolderPopup ) {
        ::MMM::UI::FeedbackOpenPopup(newFolderTitle.c_str());
        m_shouldOpenNewFolderPopup = false;
    }
    renderNamePopup(newFolderTitle.c_str(),
                    TR("ui.file_manager.new_folder_label").data(),
                    TR("ui.file_manager.context.new_folder").data(),
                    [&]() { confirmNewFolder(); });

    // 删除弹窗不需要文本输入，单独展示目标 basename 和确认按钮。
    const std::string deleteTitle =
        fmt::format("{}###FileManagerDeletePopup",
                    TR("ui.file_manager.delete_title").data());
    if ( m_shouldOpenDeletePopup ) {
        // 请求由条目右键菜单设置，只消费一次。
        ::MMM::UI::FeedbackOpenPopup(deleteTitle.c_str());
        m_shouldOpenDeletePopup = false;
    }
    bool                           openDelete = true;
    Utils::CenteredModalPopupScope deleteModalScope(dpiScale);
    if ( deleteModalScope.begin(deleteTitle.c_str(),
                                &openDelete,
                                ImGuiWindowFlags_NoCollapse,
                                { 420.0f * dpiScale, 0.0f }) ) {
        ImGui::TextWrapped(
            "%s",
            TR_FMT("ui.file_manager.delete_confirm_fmt",
                   Config::pathToUtf8(m_pendingDeletePath.filename()))
                .c_str());
        if ( !m_fileOperationError.empty() ) {
            // 音频约束或文件系统错误显示在确认文本下方。
            ImGui::TextDisabled("%s", m_fileOperationError.c_str());
        }
        const ImVec2 buttonSize{ 128.0f * dpiScale, 0.0f };
        if ( ::MMM::UI::FeedbackButton(TR("ui.common.delete").data(),
                                       buttonSize) ) {
            confirmDelete();
        }
        ImGui::SameLine();
        if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                       buttonSize) ) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/// @brief 激活谱面或音频文件树条目。
/// @param entry 当前目录快照中的文件条目。
/// @param sourceManager 用于切换侧栏并打开音轨控制器。
///
/// 谱面先切到谱面浏览器，必要时同步进项目清单，再从文件装载并创建会话；音频先
/// 切到音频浏览器，仅在项目资源表精确匹配相对路径时打开对应控制器。目录无动作。
/// @warning 低频用户交互：谱面激活会执行文件读取，只由明确点击触发。
void FileManagerView::activateFileEntry(const DirectoryEntryInfo& entry,
                                        UIManager*                sourceManager)
{
    if ( entry.isDirectory ) {
        // 目录展开由 TreeNode open 状态处理，不创建编辑会话。
        return;
    }

    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    if ( !project ) {
        // 项目切换期间不能解析相对路径或资源表。
        return;
    }

    // 优先生成项目根相对路径供 m_beatmaps 和 m_audioResources 比较。
    std::error_code filesystemError;
    auto            relP = std::filesystem::relative(
        entry.path, project->m_projectRoot, filesystemError);
    if ( filesystemError ) {
        // 相对转换失败时以 basename 兼容旧项目记录。
        relP = entry.path.filename();
    }
    const std::string relPath = Config::pathToUtf8(relP);

    // 侧栏切换事件统一指定 SideBarManager 和目标子视图 ID。
    auto publishToggleEvent = [&](SideBarTab tab) {
        Event::UISubViewToggleEvent evt;
        evt.sourceUiName           = m_subViewName;
        evt.uiManager              = sourceManager;
        evt.targetFloatManagerName = "SideBarManager";
        evt.subViewId              = TabToSubViewId(tab);
        evt.showSubView            = true;
        Event::EventBus::instance().publish(evt);
    };

    if ( isBeatmapExtension(entry.extension) ) {
        // 先显示谱面浏览器，让新会话与项目列表反馈保持一致。
        publishToggleEvent(SideBarTab::BeatMapExplorer);
        bool        foundBeatmap = false;
        std::string displayName  = entry.filename;
        for ( const auto& bm : project->m_beatmaps ) {
            if ( bm.m_filePath == relPath ) {
                foundBeatmap = true;
                displayName  = bm.m_name;
                break;
            }
        }
        if ( !foundBeatmap ) {
            // 文件树发现未登记谱面时先让项目模型吸收该文件。
            engine.syncProjectWithFile(entry.path);
            for ( const auto& bm : project->m_beatmaps ) {
                if ( bm.m_filePath == relPath ) {
                    displayName = bm.m_name;
                    break;
                }
            }
        }

        // 文件解析结果由 shared_ptr 交给新会话共享所有权。
        auto loadedBeatmap = std::make_shared<MMM::BeatMap>(
            MMM::BeatMap::loadFromFile(entry.path));
        engine.createSession(loadedBeatmap, displayName);
    } else if ( isAudioExtension(entry.extension) ) {
        // 音频激活展示音频资源侧栏。
        publishToggleEvent(SideBarTab::AudioExplorer);
        for ( const auto& audio : project->m_audioResources ) {
            if ( audio.m_path == relPath ) {
                // 控制器类型由项目资源的主/效果音轨分类映射。
                sourceManager->openAudioTrackController(
                    audio.m_id,
                    audio.m_id,
                    audio.m_type == AudioTrackType::Main
                        ? AudioTrackControllerUI::TrackType::Main
                        : AudioTrackControllerUI::TrackType::Effect);
                break;
            }
        }
    }
}

}  // namespace MMM::UI
