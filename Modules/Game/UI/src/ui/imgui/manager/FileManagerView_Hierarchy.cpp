#ifndef IMGUI_DEFINE_MATH_OPERATORS
#    define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "imgui.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ProjectResourceService.h"
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
#include <mutex>
#include <optional>
#include <system_error>

namespace MMM::UI
{
namespace
{
/// @brief 表格列编号。
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
std::string toLowerAscii(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            if ( ch >= 'A' && ch <= 'Z' ) {
                return static_cast<char>(ch - 'A' + 'a');
            }
            return static_cast<char>(ch);
        });
    return value;
}

/// @brief 判断扩展名是否为谱面文件。
/// @param extension UTF-8 扩展名。
/// @return 是支持的谱面扩展名时返回 true。
bool isBeatmapExtension(const std::string& extension)
{
    const auto ext = toLowerAscii(extension);
    return ext == ".osu" || ext == ".imd" || ext == ".mc" || ext == ".mmm";
}

/// @brief 判断扩展名是否为音频文件。
/// @param extension UTF-8 扩展名。
/// @return 是支持的音频扩展名时返回 true。
bool isAudioExtension(const std::string& extension)
{
    const auto ext = toLowerAscii(extension);
    return ext == ".mp3" || ext == ".wav" || ext == ".ogg" || ext == ".flac" ||
           ext == ".opus" || ext == ".aac" || ext == ".m4a";
}

/// @brief 文件或目录移动后同步其中音频资源的项目路径。
/// @param oldPath 移动前路径。
/// @param newPath 移动后路径。
/// @warning 低频文件操作路径：只在用户确认移动或重命名后扫描项目音频资源。
void syncMovedProjectAudioResourcePaths(const std::filesystem::path& oldPath,
                                        const std::filesystem::path& newPath)
{
    auto& engine = Logic::EditorEngine::instance();
    std::lock_guard<std::recursive_mutex> lock(engine.getSessionMutex());
    auto*                                 project = engine.getCurrentProject();
    if ( !project ) return;

    const auto changedCount =
        Logic::ProjectResourceService::remapAudioResourcePathsAfterMove(
            *project, oldPath, newPath);
    if ( changedCount == 0 ) return;

    XINFO("Updated {} project audio resource path(s) after move", changedCount);
    engine.saveProject();
}

/// @brief 计算目录直属项目数量。
/// @param path 需要统计的目录。
/// @return 成功时返回数量；失败时为空。
std::optional<std::uintmax_t> countDirectoryChildren(
    const std::filesystem::path& path)
{
    std::error_code filesystemError;
    constexpr auto  options =
        std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::directory_iterator iterator(
        path, options, filesystemError);
    if ( filesystemError ) {
        return std::nullopt;
    }

    std::uintmax_t                            count = 0;
    const std::filesystem::directory_iterator endIterator;
    while ( iterator != endIterator ) {
        ++count;
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
std::string formatFileSize(std::uintmax_t size)
{
    constexpr double kibi = 1024.0;
    constexpr double mebi = kibi * 1024.0;
    constexpr double gibi = mebi * 1024.0;

    if ( size < 1024 ) {
        return TR_FMT("ui.file_manager.size_bytes", size);
    }
    const double value = static_cast<double>(size);
    if ( value < mebi ) {
        return TR_FMT("ui.file_manager.size_kib", value / kibi);
    }
    if ( value < gibi ) {
        return TR_FMT("ui.file_manager.size_mib", value / mebi);
    }
    return TR_FMT("ui.file_manager.size_gib", value / gibi);
}

/// @brief 生成大小列显示文本。
/// @param entry 文件树条目。
/// @return 目录项目数或文件大小文本。
std::string formatSizeColumn(const FileManagerView::DirectoryEntryInfo& entry)
{
    if ( entry.isDirectory ) {
        if ( !entry.hasDirectoryChildCount ) {
            return TR("ui.file_manager.value_unknown").data();
        }
        return TR_FMT("ui.file_manager.directory_item_count",
                      entry.directoryChildCount);
    }

    if ( !entry.hasFileSize ) {
        return TR("ui.file_manager.value_unknown").data();
    }
    return formatFileSize(entry.fileSize);
}

/// @brief 生成类型列显示文本。
/// @param entry 文件树条目。
/// @return 目录、扩展名或普通文件类型文本。
std::string formatTypeColumn(const FileManagerView::DirectoryEntryInfo& entry)
{
    if ( entry.isDirectory ) {
        return TR("ui.file_manager.type_directory").data();
    }
    if ( entry.extension.empty() ) {
        return TR("ui.file_manager.type_file").data();
    }
    std::string extension = entry.extension;
    if ( extension.size() > 1 && extension.front() == '.' ) {
        extension.erase(extension.begin());
    }
    return extension.empty() ? TR("ui.file_manager.type_file").data()
                             : toLowerAscii(extension);
}

/// @brief 将文件系统时间转换为本地时间文本。
/// @param time 文件系统时间。
/// @return 本地时间文本，格式为 yyyy/mm/dd HH:MM。
std::string formatModifiedTime(std::filesystem::file_time_type time)
{
    const auto systemTime =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            time - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now());
    const std::time_t timeValue =
        std::chrono::system_clock::to_time_t(systemTime);

    std::tm localTime{};
#ifdef _WIN32
    localtime_s(&localTime, &timeValue);
#else
    localtime_r(&timeValue, &localTime);
#endif

    char buffer[32]{};
    if ( std::strftime(buffer, sizeof(buffer), "%Y/%m/%d %H:%M", &localTime) ==
         0 ) {
        return TR("ui.file_manager.value_unknown").data();
    }
    return buffer;
}

/// @brief 生成修改时间列显示文本。
/// @param entry 文件树条目。
/// @return 本地修改时间或未知占位。
std::string formatModifiedColumn(
    const FileManagerView::DirectoryEntryInfo& entry)
{
    if ( !entry.hasLastWriteTime ) {
        return TR("ui.file_manager.value_unknown").data();
    }
    return formatModifiedTime(entry.lastWriteTime);
}

/// @brief 选择文件树条目图标。
/// @param entry 文件树条目。
/// @return 图标 UTF-8 文本。
const char* fileEntryIcon(const FileManagerView::DirectoryEntryInfo& entry)
{
    if ( entry.isDirectory ) {
        return ICON_MMM_FOLDER;
    }
    if ( isAudioExtension(entry.extension) ) {
        return ICON_MMM_MUSIC;
    }
    return ICON_MMM_FILE;
}

/// @brief 生成目录缓存键。
/// @param path 目录路径。
/// @return UTF-8 缓存键。
std::string directoryCacheKey(const std::filesystem::path& path)
{
    return Config::pathToUtf8(path.lexically_normal());
}

/// @brief 比较两个可选数值。
/// @tparam T 可比较数值类型。
/// @param lhs 左值。
/// @param lhsValid 左值是否有效。
/// @param rhs 右值。
/// @param rhsValid 右值是否有效。
/// @return 小于返回 -1，大于返回 1，相等返回 0。
template<typename T>
int compareOptionalValue(const T& lhs, bool lhsValid, const T& rhs,
                         bool rhsValid)
{
    if ( lhsValid != rhsValid ) {
        return lhsValid ? -1 : 1;
    }
    if ( !lhsValid ) {
        return 0;
    }
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
bool fileEntryLess(const FileManagerView::DirectoryEntryInfo& lhs,
                   const FileManagerView::DirectoryEntryInfo& rhs,
                   FileManagerView::FileSortKey               sortKey,
                   FileManagerView::SortDirection             sortDirection,
                   bool                                       directoriesFirst)
{
    if ( directoriesFirst && lhs.isDirectory != rhs.isDirectory ) {
        return lhs.isDirectory;
    }

    int compareResult = 0;
    switch ( sortKey ) {
    case FileManagerView::FileSortKey::Name:
        compareResult =
            toLowerAscii(lhs.filename).compare(toLowerAscii(rhs.filename));
        break;
    case FileManagerView::FileSortKey::Type:
        compareResult =
            toLowerAscii(lhs.extension).compare(toLowerAscii(rhs.extension));
        break;
    case FileManagerView::FileSortKey::Size:
        if ( lhs.isDirectory || rhs.isDirectory ) {
            compareResult = compareOptionalValue(lhs.directoryChildCount,
                                                 lhs.hasDirectoryChildCount,
                                                 rhs.directoryChildCount,
                                                 rhs.hasDirectoryChildCount);
        } else {
            compareResult = compareOptionalValue(
                lhs.fileSize, lhs.hasFileSize, rhs.fileSize, rhs.hasFileSize);
        }
        break;
    case FileManagerView::FileSortKey::ModifiedTime:
        compareResult = compareOptionalValue(lhs.lastWriteTime,
                                             lhs.hasLastWriteTime,
                                             rhs.lastWriteTime,
                                             rhs.hasLastWriteTime);
        break;
    }

    if ( compareResult == 0 ) {
        compareResult =
            toLowerAscii(lhs.filename).compare(toLowerAscii(rhs.filename));
    }
    if ( compareResult == 0 ) {
        compareResult =
            toLowerAscii(lhs.fullPath).compare(toLowerAscii(rhs.fullPath));
    }

    if ( sortDirection == FileManagerView::SortDirection::Descending ) {
        compareResult = -compareResult;
    }
    return compareResult < 0;
}

/// @brief 组合排序菜单项显示文本。
/// @param columnLabel 排序字段显示名。
/// @param direction 排序方向。
/// @return 带方向后缀的菜单文本。
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
bool isTableColumnEnabled(const ImGuiTable* table, int column)
{
    return table && column >= 0 && column < table->ColumnsCount &&
           table->Columns[column].IsEnabled;
}

/// @brief 查询表格列的用户显隐状态。
/// @param table ImGui 表格指针。
/// @param column 列索引。
/// @return 用户设置为显示时返回 true。
bool isTableColumnUserEnabled(const ImGuiTable* table, int column)
{
    return table && column >= 0 && column < table->ColumnsCount &&
           table->Columns[column].IsUserEnabled;
}

/// @brief 排队设置表格列下一帧的用户显隐状态。
/// @param table ImGui 表格指针。
/// @param column 列索引。
/// @param enabled 是否显示。
void queueTableColumnEnabled(ImGuiTable* table, int column, bool enabled)
{
    if ( !table || column < 0 || column >= table->ColumnsCount ) {
        return;
    }
    table->Columns[column].IsUserEnabledNextFrame = enabled;
}

/// @brief 判断路径是否存在。
/// @param path 需要检查的路径。
/// @return 存在且没有文件系统错误时返回 true。
bool filesystemPathExists(const std::filesystem::path& path)
{
    std::error_code filesystemError;
    const bool      exists = std::filesystem::exists(path, filesystemError);
    return exists && !filesystemError;
}

/// @brief 判断路径是否为目录。
/// @param path 需要检查的路径。
/// @return 是目录且没有文件系统错误时返回 true。
bool filesystemPathIsDirectory(const std::filesystem::path& path)
{
    std::error_code filesystemError;
    const bool      isDirectory =
        std::filesystem::is_directory(path, filesystemError);
    return isDirectory && !filesystemError;
}

/// @brief 获取用于父子关系判断的规范化路径。
/// @param path 输入路径。
/// @return 尽量规范化后的路径。
std::filesystem::path comparableFilesystemPath(
    const std::filesystem::path& path)
{
    std::error_code filesystemError;
    auto            canonicalPath =
        std::filesystem::weakly_canonical(path, filesystemError);
    if ( !filesystemError ) {
        return canonicalPath.lexically_normal();
    }
    return path.lexically_normal();
}

/// @brief 判断路径是否位于指定根目录内。
/// @param root 根目录。
/// @param path 待检查路径。
/// @return path 等于 root 或位于 root 内时返回 true。
bool pathIsInsideOrSame(const std::filesystem::path& root,
                        const std::filesystem::path& path)
{
    const auto normalizedRoot = comparableFilesystemPath(root);
    const auto normalizedPath = comparableFilesystemPath(path);
    auto       rootIt         = normalizedRoot.begin();
    auto       pathIt         = normalizedPath.begin();
    for ( ; rootIt != normalizedRoot.end(); ++rootIt, ++pathIt ) {
        if ( pathIt == normalizedPath.end() || *rootIt != *pathIt ) {
            return false;
        }
    }
    return true;
}

/// @brief 判断文件名输入是否包含路径分隔符。
/// @param filename UTF-8 文件名输入。
/// @return 含路径分隔符时返回 true。
bool containsPathSeparator(const std::string& filename)
{
    return filename.find('/') != std::string::npos ||
           filename.find('\\') != std::string::npos;
}

/// @brief 判断文件名输入是否可用于单个文件系统条目。
/// @param filename UTF-8 文件名输入。
/// @return 文件名有效时返回 true。
bool isValidSingleFilename(const std::string& filename)
{
    if ( filename.empty() || filename == "." || filename == ".." ) {
        return false;
    }
    return !containsPathSeparator(filename);
}

/// @brief 为复制或移动生成不覆盖既有文件的目标路径。
/// @param source 源路径。
/// @param targetDirectory 目标目录。
/// @return 唯一目标路径。
std::filesystem::path makeUniqueDestinationPath(
    const std::filesystem::path& source,
    const std::filesystem::path& targetDirectory)
{
    const bool sourceIsDirectory = filesystemPathIsDirectory(source);
    const auto filename          = source.filename();
    auto       candidate         = targetDirectory / filename;
    if ( !filesystemPathExists(candidate) ) {
        return candidate;
    }

    const auto filenameText = Config::pathToUtf8(filename);
    const auto stemText =
        sourceIsDirectory ? filenameText : Config::pathToUtf8(source.stem());
    const auto extensionText = sourceIsDirectory
                                   ? std::string{}
                                   : Config::pathToUtf8(source.extension());
    for ( int index = 1; index < 10000; ++index ) {
        const std::string suffix =
            index == 1 ? TR("ui.file_manager.copy_suffix").data()
                       : TR_FMT("ui.file_manager.copy_suffix_numbered", index);
        candidate = targetDirectory /
                    Config::utf8ToPath(stemText + suffix + extensionText);
        if ( !filesystemPathExists(candidate) ) {
            return candidate;
        }
    }
    return targetDirectory /
           Config::utf8ToPath(stemText + " copy" + extensionText);
}

/// @brief 将文件系统错误格式化为用户可读文本。
/// @param action 操作名称。
/// @param error 文件系统错误码。
/// @return 格式化后的错误文本。
std::string formatFilesystemError(const char*            action,
                                  const std::error_code& error)
{
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
bool copyFilesystemEntry(const std::filesystem::path& source,
                         const std::filesystem::path& destination,
                         std::error_code&             error)
{
    error.clear();
    std::filesystem::copy(
        source, destination, std::filesystem::copy_options::recursive, error);
    return !error;
}
}  // namespace

void FileManagerView::renderActiveProjectView(LayoutContext& layoutContext,
                                              UIManager*     sourceManager)
{
    auto& skinCfg = Config::SkinManager::instance();

    const std::filesystem::path projectRoot = m_currentRoot;

    const float dpiScale =
        std::max(1.0f, Config::AppConfig::instance().getWindowContentScale());
    const auto& style          = ImGui::GetStyle();
    auto        toLayoutPixels = [](float value) {
        return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
    };
    // 与谱面管理器的原生相邻项布局保持一致，避免 Header 和表格过近。
    const uint16_t itemSpacing = toLayoutPixels(style.ItemSpacing.y);
    const uint16_t rootPadding = toLayoutPixels(std::min(
        4.0f * dpiScale, std::max(0.0f, layoutContext.m_avail.x) * 0.02f));

    CLayVBox treeVBox;
    treeVBox.setSpacing(itemSpacing);

    // 1. Root 节点作为 CollapsingHeader
    treeVBox.addElement(
        "ProjectRootHeader",
        Sizing::Grow(),
        Sizing::Fixed(ImGui::GetFrameHeight()),
        [this, projectRoot](Clay_BoundingBox r, bool isHovered) {
            std::string rootName = Config::pathToUtf8(projectRoot.filename());
            Utils::renderScrollingCollapsingHeader(
                "ProjectRootHeader", rootName, &m_showRoot, r);
            if ( ImGui::IsItemHovered() ) {
                std::string fullPath = Config::pathToUtf8(projectRoot);
                ImGui::SetTooltip("%s", fullPath.c_str());
            }
        });

    if ( m_showRoot ) {
        treeVBox.addElement(
            "FileTree",
            Sizing::Grow(),
            Sizing::Grow(),
            [this, sourceManager, dpiScale](Clay_BoundingBox r,
                                            bool             isHovered) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                {
                    Utils::VerticalScrollbarStyleScope verticalScrollbarStyle(
                        dpiScale);
                    const ImGuiTableFlags tableFlags =
                        ImGuiTableFlags_BordersV |
                        ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_Resizable |
                        ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                        ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_SizingStretchProp;
                    const auto& tableStyle = ImGui::GetStyle();
                    const float fontScale  = ImGui::GetFontSize() / 17.0f;
                    const float sizeColumnWidth =
                        std::max(96.0f, 104.0f * fontScale);
                    const float typeColumnWidth =
                        std::max(76.0f, 86.0f * fontScale);
                    const float modifiedColumnPreferredWidth =
                        std::max(168.0f,
                                 ImGui::CalcTextSize("0000/00/00 00:00").x +
                                     tableStyle.CellPadding.x * 4.0f +
                                     ImGui::GetFrameHeight());
                    const float nameColumnMinWidth =
                        std::max(148.0f,
                                 ImGui::CalcTextSize(
                                     TR("ui.file_manager.column_name").data())
                                         .x +
                                     tableStyle.CellPadding.x * 4.0f +
                                     ImGui::GetFrameHeight());
                    const float tableReserveWidth =
                        tableStyle.ScrollbarSize +
                        tableStyle.CellPadding.x * 2.0f;
                    const float nameColumnWidth = std::max(
                        nameColumnMinWidth,
                        r.width - typeColumnWidth - sizeColumnWidth -
                            modifiedColumnPreferredWidth - tableReserveWidth);

                    if ( ImGui::BeginTable("FileTreeTableV2",
                                           4,
                                           tableFlags,
                                           { r.width, r.height }) ) {
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
                            table->DisableDefaultContextMenu = true;
                        }
                        ImGui::TableHeadersRow();
                        syncFileTableSortSpecs();

                        const float indent = ImGui::CalcTextSize("AA").x;
                        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing,
                                            indent);
                        drawDirectoryRecursive(m_currentRoot, sourceManager);
                        ImGui::PopStyleVar();

                        renderFileSortContextMenu();
                        ImGui::EndTable();
                    }
                }
                renderFileBackgroundContextMenu(sourceManager);
            });
    }

    CLayVBox rootVBox;
    rootVBox.setPadding(rootPadding, rootPadding, rootPadding, rootPadding)
        .setSpacing(itemSpacing)
        .addLayout("treeVBox", treeVBox, Sizing::Grow(), Sizing::Grow());

    rootVBox.render(layoutContext);
}

void FileManagerView::drawDirectoryRecursive(const std::filesystem::path& path,
                                             UIManager* sourceManager)
{
    const auto& snapshot = getDirectorySnapshot(path);
    const float dpiScale =
        std::max(1.0f, Config::AppConfig::instance().getWindowContentScale());
    const float itemH = std::max(24.0f * dpiScale, ImGui::GetFrameHeight());

    for ( const auto& entry : snapshot.entries ) {
        ImGui::TableNextRow(ImGuiTableRowFlags_None, itemH);

        ImGui::TableNextColumn();
        const std::string displayName =
            fmt::format("{}  {}", fileEntryIcon(entry), entry.filename);
        const float  nameColumnWidth = ImGui::GetContentRegionAvail().x;
        const ImVec4 transparent{ 0.0f, 0.0f, 0.0f, 0.0f };
        ImGui::PushStyleColor(ImGuiCol_Header, transparent);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, transparent);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, transparent);
        const bool open = Utils::renderScrollingTreeNode(
            entry.fullPath,
            displayName,
            nameColumnWidth,
            itemH,
            !entry.isDirectory,
            [this, &entry, sourceManager]() {
                activateFileEntry(entry, sourceManager);
            },
            "");
        ImGui::PopStyleColor(3);

        // 以 ImGui 表格行边界和首列结算高度判定 Hover，避免手算行高偏差。
        const ImGuiTable* table  = ImGui::GetCurrentTable();
        const float       mouseY = ImGui::GetMousePos().y;
        const float       rowMaxY =
            table ? std::max(table->RowPosY2,
                             ImGui::GetItemRectMax().y + table->RowCellPaddingY)
                  : 0.0f;
        const bool rowHovered = table && ImGui::TableGetHoveredColumn() >= 0 &&
                                mouseY >= table->RowPosY1 && mouseY < rowMaxY;
        if ( rowHovered ) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                   ImGui::GetColorU32(ImGuiCol_HeaderHovered));
        }
        renderFileEntryContextMenu(entry, sourceManager);
        if ( rowHovered ) {
            Utils::renderTooltip(entry.fullPath.c_str());
        }

        if ( ImGui::TableNextColumn() ) {
            const std::string typeText = formatTypeColumn(entry);
            ImGui::TextUnformatted(typeText.c_str());
        }

        ImGui::TableNextColumn();
        const std::string sizeText = formatSizeColumn(entry);
        ImGui::TextUnformatted(sizeText.c_str());

        ImGui::TableNextColumn();
        const std::string modifiedText = formatModifiedColumn(entry);
        ImGui::TextUnformatted(modifiedText.c_str());

        if ( entry.isDirectory && open ) {
            drawDirectoryRecursive(entry.path, sourceManager);
            ImGui::TreePop();
        }
    }
}

const FileManagerView::DirectorySnapshot& FileManagerView::getDirectorySnapshot(
    const std::filesystem::path& path)
{
    const auto cacheKey = directoryCacheKey(path);
    if ( const auto it = m_directoryCache.find(cacheKey);
         it != m_directoryCache.end() && it->second.sortKey == m_fileSortKey &&
         it->second.sortDirection == m_fileSortDirection &&
         it->second.directoriesFirst == m_directoriesFirst ) {
        return it->second;
    }

    DirectorySnapshot snapshot;
    snapshot.sortKey          = m_fileSortKey;
    snapshot.sortDirection    = m_fileSortDirection;
    snapshot.directoriesFirst = m_directoriesFirst;

    std::error_code filesystemError;
    constexpr auto  options =
        std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::directory_iterator iterator(
        path, options, filesystemError);
    const std::filesystem::directory_iterator endIterator;
    while ( !filesystemError && iterator != endIterator ) {
        const auto         entryPath = iterator->path();
        DirectoryEntryInfo info;
        info.path      = entryPath;
        info.filename  = Config::pathToUtf8(entryPath.filename());
        info.fullPath  = Config::pathToUtf8(entryPath);
        info.extension = Config::pathToUtf8(entryPath.extension());

        std::error_code entryError;
        info.isDirectory = iterator->is_directory(entryError) && !entryError;
        entryError.clear();
        info.isRegularFile =
            iterator->is_regular_file(entryError) && !entryError;

        if ( info.isDirectory ) {
            if ( auto childCount = countDirectoryChildren(entryPath) ) {
                info.directoryChildCount    = *childCount;
                info.hasDirectoryChildCount = true;
            }
        } else if ( info.isRegularFile ) {
            entryError.clear();
            const auto fileSize = iterator->file_size(entryError);
            if ( !entryError ) {
                info.fileSize    = fileSize;
                info.hasFileSize = true;
            }
        }

        entryError.clear();
        const auto modifiedTime = iterator->last_write_time(entryError);
        if ( !entryError ) {
            info.lastWriteTime    = modifiedTime;
            info.hasLastWriteTime = true;
        }

        snapshot.entries.push_back(std::move(info));
        iterator.increment(filesystemError);
    }

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

    auto [it, inserted] =
        m_directoryCache.insert_or_assign(cacheKey, std::move(snapshot));
    return it->second;
}

void FileManagerView::invalidateDirectoryCache()
{
    m_directoryCache.clear();
}

void FileManagerView::consumePendingDirectoryRefreshes()
{
    bool hasRefreshRequest = false;
    bool refreshRequest    = false;
    while ( m_pendingDirectoryRefreshes.try_dequeue(refreshRequest) ) {
        hasRefreshRequest = true;
    }

    if ( hasRefreshRequest ) {
        invalidateDirectoryCache();
    }
}

void FileManagerView::syncFileTableSortSpecs()
{
    ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
    if ( !sortSpecs || sortSpecs->SpecsCount <= 0 || !sortSpecs->SpecsDirty ) {
        return;
    }

    const ImGuiTableColumnSortSpecs& primarySpec = sortSpecs->Specs[0];
    FileSortKey                      newSortKey  = FileSortKey::Name;
    if ( primarySpec.ColumnIndex == FileTableColumnType ) {
        newSortKey = FileSortKey::Type;
    } else if ( primarySpec.ColumnIndex == FileTableColumnSize ) {
        newSortKey = FileSortKey::Size;
    } else if ( primarySpec.ColumnIndex == FileTableColumnModifiedTime ) {
        newSortKey = FileSortKey::ModifiedTime;
    }

    const SortDirection newDirection =
        primarySpec.SortDirection == ImGuiSortDirection_Descending
            ? SortDirection::Descending
            : SortDirection::Ascending;

    if ( newSortKey != m_fileSortKey || newDirection != m_fileSortDirection ) {
        m_fileSortKey       = newSortKey;
        m_fileSortDirection = newDirection;
        invalidateDirectoryCache();
    }
    sortSpecs->SpecsDirty = false;
}

void FileManagerView::renderFileSortContextMenu()
{
    ImGuiTable* table = ImGui::GetCurrentTable();
    if ( !table ) {
        return;
    }

    ImGuiStyle&  style = ImGui::GetStyle();
    const ImVec2 popupPadding(std::max(style.WindowPadding.x, 8.0f),
                              std::max(style.WindowPadding.y, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popupPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(std::max(style.ItemSpacing.x, 8.0f),
                               std::max(style.ItemSpacing.y, 4.0f)));
    const bool popupOpen = ImGui::TableBeginContextMenuPopup(table);
    if ( !popupOpen ) {
        ImGui::PopStyleVar(2);
        return;
    }

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

    auto applySort = [&](FileSortKey sortKey, SortDirection direction) {
        if ( m_fileSortKey != sortKey || m_fileSortDirection != direction ) {
            m_fileSortKey       = sortKey;
            m_fileSortDirection = direction;
            invalidateDirectoryCache();
        }
    };
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

void FileManagerView::renderFileBackgroundContextMenu(UIManager* sourceManager)
{
    constexpr ImGuiPopupFlags popupFlags =
        ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems;
    if ( !ImGui::BeginPopupContextWindow("FileTreeBackgroundContextMenu",
                                         popupFlags) ) {
        return;
    }

    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.new_beatmap").data()) ) {
        openNewBeatmapWizard(sourceManager);
    }
    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.new_folder").data()) ) {
        requestNewFolder(m_currentRoot);
    }

    const bool canPaste = hasPasteableFileClipboard();
    if ( ::MMM::UI::FeedbackMenuItem(TR("ui.file_manager.context.paste").data(),
                                     nullptr,
                                     false,
                                     canPaste) ) {
        pasteFileClipboardInto(m_currentRoot);
    }

    ImGui::Separator();
    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.refresh").data()) ) {
        invalidateDirectoryCache();
    }

    ImGui::EndPopup();
}

void FileManagerView::renderFileEntryContextMenu(
    const DirectoryEntryInfo& entry, UIManager* sourceManager)
{
    const std::string popupId = "FileEntryContext_" + entry.fullPath;
    if ( !ImGui::BeginPopupContextItem(popupId.c_str(),
                                       ImGuiPopupFlags_MouseButtonRight) ) {
        return;
    }

    const auto targetDirectory =
        entry.isDirectory ? entry.path : entry.path.parent_path();

    if ( !entry.isDirectory && (isBeatmapExtension(entry.extension) ||
                                isAudioExtension(entry.extension)) ) {
        if ( ::MMM::UI::FeedbackMenuItem(
                 TR("ui.file_manager.context.open").data()) ) {
            activateFileEntry(entry, sourceManager);
        }
    }

    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.new_beatmap").data()) ) {
        openNewBeatmapWizard(sourceManager);
    }
    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.new_folder").data()) ) {
        requestNewFolder(targetDirectory);
    }

    const bool canPaste = hasPasteableFileClipboard();
    if ( ::MMM::UI::FeedbackMenuItem(TR("ui.file_manager.context.paste").data(),
                                     nullptr,
                                     false,
                                     canPaste) ) {
        pasteFileClipboardInto(targetDirectory);
    }

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
        m_directoryCache.erase(directoryCacheKey(entry.path));
    }

    if ( ::MMM::UI::FeedbackMenuItem(
             TR("ui.file_manager.context.open_path").data()) ) {
        if ( !DesktopPathUtils::openInFileManager(entry.path,
                                                  !entry.isDirectory) ) {
            XERROR("无法在系统文件管理器中打开路径：{}", entry.fullPath);
        }
    }

    ImGui::EndPopup();
}

void FileManagerView::openNewBeatmapWizard(UIManager* sourceManager)
{
    if ( !sourceManager ) {
        return;
    }

    auto* wizard = sourceManager->getView<NewBeatmapWizard>("NewBeatmapWizard");
    if ( wizard ) {
        wizard->open();
    }
}

void FileManagerView::setFileOperationInput(const std::string& value)
{
    m_fileOperationInput.fill('\0');
    const auto copySize =
        std::min(m_fileOperationInput.size() - 1, value.size());
    std::memcpy(m_fileOperationInput.data(), value.data(), copySize);
    m_fileOperationInput[copySize] = '\0';
}

void FileManagerView::requestRename(const std::filesystem::path& path)
{
    m_pendingRenamePath = path;
    m_fileOperationError.clear();
    setFileOperationInput(Config::pathToUtf8(path.filename()));
    m_shouldOpenRenamePopup         = true;
    m_shouldFocusFileOperationInput = true;
}

void FileManagerView::requestNewFolder(const std::filesystem::path& directory)
{
    m_pendingNewFolderDirectory = directory;
    m_fileOperationError.clear();
    setFileOperationInput(TR("ui.file_manager.default_new_folder_name").data());
    m_shouldOpenNewFolderPopup      = true;
    m_shouldFocusFileOperationInput = true;
}

void FileManagerView::requestDelete(const std::filesystem::path& path)
{
    m_pendingDeletePath = path;
    m_fileOperationError.clear();
    m_shouldOpenDeletePopup = true;
}

void FileManagerView::setFileClipboard(const std::filesystem::path& path,
                                       bool                         cut)
{
    if ( !filesystemPathExists(path) ||
         !pathIsInsideOrSame(m_currentRoot, path) ) {
        m_fileClipboardPath.clear();
        m_fileClipboardMode = FileClipboardMode::None;
        return;
    }

    m_fileClipboardPath = path;
    m_fileClipboardMode =
        cut ? FileClipboardMode::Cut : FileClipboardMode::Copy;
}

bool FileManagerView::hasPasteableFileClipboard() const
{
    return m_fileClipboardMode != FileClipboardMode::None &&
           filesystemPathExists(m_fileClipboardPath) &&
           pathIsInsideOrSame(m_currentRoot, m_fileClipboardPath);
}

void FileManagerView::pasteFileClipboardInto(
    const std::filesystem::path& targetDirectory)
{
    m_fileOperationError.clear();
    if ( !hasPasteableFileClipboard() ) {
        m_fileOperationError =
            TR("ui.file_manager.error_clipboard_empty").data();
        return;
    }
    if ( !filesystemPathIsDirectory(targetDirectory) ||
         !pathIsInsideOrSame(m_currentRoot, targetDirectory) ) {
        m_fileOperationError =
            TR("ui.file_manager.error_target_not_directory").data();
        return;
    }
    if ( filesystemPathIsDirectory(m_fileClipboardPath) &&
         pathIsInsideOrSame(m_fileClipboardPath, targetDirectory) ) {
        m_fileOperationError =
            TR("ui.file_manager.error_paste_into_self").data();
        return;
    }

    const auto destination =
        makeUniqueDestinationPath(m_fileClipboardPath, targetDirectory);
    std::error_code filesystemError;
    if ( m_fileClipboardMode == FileClipboardMode::Cut ) {
        const auto movedSourcePath = m_fileClipboardPath;
        std::filesystem::rename(
            m_fileClipboardPath, destination, filesystemError);
        if ( filesystemError ) {
            filesystemError.clear();
            if ( copyFilesystemEntry(
                     m_fileClipboardPath, destination, filesystemError) ) {
                std::error_code removeError;
                if ( filesystemPathIsDirectory(m_fileClipboardPath) ) {
                    std::filesystem::remove_all(m_fileClipboardPath,
                                                removeError);
                } else {
                    std::filesystem::remove(m_fileClipboardPath, removeError);
                }
                filesystemError = removeError;
            }
        }
        if ( filesystemError ) {
            m_fileOperationError = formatFilesystemError(
                TR("ui.file_manager.operation_paste").data(), filesystemError);
            XERROR("Failed to move file manager clipboard from {} to {}: {}",
                   Config::pathToUtf8(m_fileClipboardPath),
                   Config::pathToUtf8(destination),
                   filesystemError.message());
            return;
        }
        syncMovedProjectAudioResourcePaths(movedSourcePath, destination);
        m_fileClipboardPath.clear();
        m_fileClipboardMode = FileClipboardMode::None;
    } else {
        if ( !copyFilesystemEntry(
                 m_fileClipboardPath, destination, filesystemError) ) {
            m_fileOperationError = formatFilesystemError(
                TR("ui.file_manager.operation_paste").data(), filesystemError);
            XERROR("Failed to copy file manager clipboard from {} to {}: {}",
                   Config::pathToUtf8(m_fileClipboardPath),
                   Config::pathToUtf8(destination),
                   filesystemError.message());
            return;
        }
    }

    invalidateDirectoryCache();
}

void FileManagerView::confirmRename()
{
    m_fileOperationError.clear();
    const std::string newName = m_fileOperationInput.data();
    if ( !isValidSingleFilename(newName) ) {
        m_fileOperationError = TR("ui.file_manager.error_invalid_name").data();
        return;
    }
    if ( !filesystemPathExists(m_pendingRenamePath) ||
         !pathIsInsideOrSame(m_currentRoot, m_pendingRenamePath) ) {
        m_fileOperationError =
            TR("ui.file_manager.error_missing_source").data();
        return;
    }

    const auto destination =
        m_pendingRenamePath.parent_path() / Config::utf8ToPath(newName);
    if ( destination == m_pendingRenamePath ) {
        ImGui::CloseCurrentPopup();
        return;
    }
    if ( filesystemPathExists(destination) ) {
        m_fileOperationError = TR("ui.file_manager.error_target_exists").data();
        return;
    }
    if ( !pathIsInsideOrSame(m_currentRoot, destination.parent_path()) ) {
        m_fileOperationError =
            TR("ui.file_manager.error_outside_project").data();
        return;
    }

    std::error_code filesystemError;
    std::filesystem::rename(m_pendingRenamePath, destination, filesystemError);
    if ( filesystemError ) {
        m_fileOperationError = formatFilesystemError(
            TR("ui.file_manager.operation_rename").data(), filesystemError);
        XERROR("Failed to rename {} to {}: {}",
               Config::pathToUtf8(m_pendingRenamePath),
               Config::pathToUtf8(destination),
               filesystemError.message());
        return;
    }

    syncMovedProjectAudioResourcePaths(m_pendingRenamePath, destination);
    invalidateDirectoryCache();
    ImGui::CloseCurrentPopup();
}

void FileManagerView::confirmNewFolder()
{
    m_fileOperationError.clear();
    const std::string folderName = m_fileOperationInput.data();
    if ( !isValidSingleFilename(folderName) ) {
        m_fileOperationError = TR("ui.file_manager.error_invalid_name").data();
        return;
    }
    if ( !filesystemPathIsDirectory(m_pendingNewFolderDirectory) ||
         !pathIsInsideOrSame(m_currentRoot, m_pendingNewFolderDirectory) ) {
        m_fileOperationError =
            TR("ui.file_manager.error_target_not_directory").data();
        return;
    }

    const auto newFolderPath =
        m_pendingNewFolderDirectory / Config::utf8ToPath(folderName);
    if ( filesystemPathExists(newFolderPath) ) {
        m_fileOperationError = TR("ui.file_manager.error_target_exists").data();
        return;
    }

    std::error_code filesystemError;
    std::filesystem::create_directory(newFolderPath, filesystemError);
    if ( filesystemError ) {
        m_fileOperationError = formatFilesystemError(
            TR("ui.file_manager.operation_new_folder").data(), filesystemError);
        XERROR("Failed to create folder {}: {}",
               Config::pathToUtf8(newFolderPath),
               filesystemError.message());
        return;
    }

    invalidateDirectoryCache();
    ImGui::CloseCurrentPopup();
}

void FileManagerView::confirmDelete()
{
    m_fileOperationError.clear();
    if ( !filesystemPathExists(m_pendingDeletePath) ||
         !pathIsInsideOrSame(m_currentRoot, m_pendingDeletePath) ||
         comparableFilesystemPath(m_pendingDeletePath) ==
             comparableFilesystemPath(m_currentRoot) ) {
        m_fileOperationError =
            TR("ui.file_manager.error_missing_source").data();
        return;
    }

    std::error_code filesystemError;
    if ( filesystemPathIsDirectory(m_pendingDeletePath) ) {
        std::filesystem::remove_all(m_pendingDeletePath, filesystemError);
    } else {
        std::filesystem::remove(m_pendingDeletePath, filesystemError);
    }
    if ( filesystemError ) {
        m_fileOperationError = formatFilesystemError(
            TR("ui.file_manager.operation_delete").data(), filesystemError);
        XERROR("Failed to delete {}: {}",
               Config::pathToUtf8(m_pendingDeletePath),
               filesystemError.message());
        return;
    }

    if ( pathIsInsideOrSame(m_pendingDeletePath, m_fileClipboardPath) ) {
        m_fileClipboardPath.clear();
        m_fileClipboardMode = FileClipboardMode::None;
    }
    invalidateDirectoryCache();
    ImGui::CloseCurrentPopup();
}

void FileManagerView::renderFileOperationPopups(float dpiScale)
{
    auto renderNamePopup = [&](const char* title,
                               const char* label,
                               const char* confirmLabel,
                               auto&&      onConfirm) {
        bool                           open = true;
        Utils::CenteredModalPopupScope modalScope(dpiScale);
        if ( modalScope.begin(title,
                              &open,
                              ImGuiWindowFlags_NoCollapse,
                              { 360.0f * dpiScale, 0.0f }) ) {
            ImGui::TextUnformatted(label);
            if ( m_shouldFocusFileOperationInput ) {
                ImGui::SetKeyboardFocusHere();
                m_shouldFocusFileOperationInput = false;
            }
            const bool enterPressed =
                ImGui::InputText("##FileManagerOperationName",
                                 m_fileOperationInput.data(),
                                 m_fileOperationInput.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
            if ( !m_fileOperationError.empty() ) {
                ImGui::TextDisabled("%s", m_fileOperationError.c_str());
            }
            const ImVec2 buttonSize{ 128.0f * dpiScale, 0.0f };
            const bool   confirmClicked =
                ::MMM::UI::FeedbackButton(confirmLabel, buttonSize);
            if ( enterPressed || confirmClicked ) {
                onConfirm();
            }
            ImGui::SameLine();
            if ( ::MMM::UI::FeedbackButton(TR("ui.common.cancel").data(),
                                           buttonSize) ) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    };

    const std::string renameTitle =
        fmt::format("{}###FileManagerRenamePopup",
                    TR("ui.file_manager.rename_title").data());
    if ( m_shouldOpenRenamePopup ) {
        ::MMM::UI::FeedbackOpenPopup(renameTitle.c_str());
        m_shouldOpenRenamePopup = false;
    }
    renderNamePopup(renameTitle.c_str(),
                    TR("ui.file_manager.rename_label").data(),
                    TR("ui.file_manager.context.rename").data(),
                    [&]() { confirmRename(); });

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

    const std::string deleteTitle =
        fmt::format("{}###FileManagerDeletePopup",
                    TR("ui.file_manager.delete_title").data());
    if ( m_shouldOpenDeletePopup ) {
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

void FileManagerView::activateFileEntry(const DirectoryEntryInfo& entry,
                                        UIManager*                sourceManager)
{
    if ( entry.isDirectory ) {
        return;
    }

    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    if ( !project ) {
        return;
    }

    std::error_code filesystemError;
    auto            relP = std::filesystem::relative(
        entry.path, project->m_projectRoot, filesystemError);
    if ( filesystemError ) {
        relP = entry.path.filename();
    }
    const std::string relPath = Config::pathToUtf8(relP);

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
            engine.syncProjectWithFile(entry.path);
            for ( const auto& bm : project->m_beatmaps ) {
                if ( bm.m_filePath == relPath ) {
                    displayName = bm.m_name;
                    break;
                }
            }
        }

        auto loadedBeatmap = std::make_shared<MMM::BeatMap>(
            MMM::BeatMap::loadFromFile(entry.path));
        engine.createSession(loadedBeatmap, displayName);
    } else if ( isAudioExtension(entry.extension) ) {
        publishToggleEvent(SideBarTab::AudioExplorer);
        for ( const auto& audio : project->m_audioResources ) {
            if ( audio.m_path == relPath ) {
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
