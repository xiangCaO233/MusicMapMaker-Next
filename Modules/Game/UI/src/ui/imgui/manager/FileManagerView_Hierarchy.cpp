#include "config/AppConfig.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "event/ui/UISubViewToggleEvent.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/SideBarUI.h"
#include "ui/imgui/audio/AudioTrackControllerUI.h"
#include "ui/imgui/manager/FileManagerView.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fmt/format.h>
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

    /// @brief 大小列。
    FileTableColumnSize = 1,

    /// @brief 修改时间列。
    FileTableColumnModifiedTime = 2
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
}  // namespace

void FileManagerView::renderActiveProjectView(LayoutContext& layoutContext,
                                              UIManager*     sourceManager)
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    auto& skinCfg = Config::SkinManager::instance();

    if ( m_currentRoot != project->m_projectRoot ) {
        m_currentRoot = project->m_projectRoot;
        invalidateDirectoryCache();
    }

    const float dpiScale =
        std::max(1.0f, Config::AppConfig::instance().getWindowContentScale());
    const auto& style          = ImGui::GetStyle();
    auto        toLayoutPixels = [](float value) {
        return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
    };
    const uint16_t compactGap =
        toLayoutPixels(std::max(2.0f * dpiScale, style.ItemSpacing.y * 0.25f));
    const uint16_t rootPadding = toLayoutPixels(std::min(
        4.0f * dpiScale, std::max(0.0f, layoutContext.m_avail.x) * 0.02f));

    CLayVBox treeVBox;
    treeVBox.setSpacing(compactGap);

    // 1. Root 节点作为 CollapsingHeader
    treeVBox.addElement("ProjectRootHeader",
                        Sizing::Grow(),
                        Sizing::Fixed(ImGui::GetFrameHeight()),
                        [this, project](Clay_BoundingBox r, bool isHovered) {
                            std::string rootName = Config::pathToUtf8(
                                project->m_projectRoot.filename());
                            Utils::renderScrollingCollapsingHeader(
                                "ProjectRootHeader", rootName, &m_showRoot, r);
                            if ( ImGui::IsItemHovered() ) {
                                std::string fullPath =
                                    Config::pathToUtf8(project->m_projectRoot);
                                ImGui::SetTooltip("%s", fullPath.c_str());
                            }
                        });

    if ( m_showRoot ) {
        treeVBox.addElement(
            "FileTree",
            Sizing::Grow(),
            Sizing::Grow(),
            [this, sourceManager](Clay_BoundingBox r, bool isHovered) {
                ImGui::SetCursorScreenPos({ r.x, r.y });
                const ImGuiTableFlags tableFlags =
                    ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
                    ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
                    ImGuiTableFlags_SizingStretchProp;
                const auto& tableStyle = ImGui::GetStyle();
                const float fontScale  = ImGui::GetFontSize() / 17.0f;
                const float sizeColumnWidth =
                    std::max(96.0f, 104.0f * fontScale);
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
                    tableStyle.ScrollbarSize + tableStyle.CellPadding.x * 2.0f;
                const float nameColumnWidth = std::max(
                    nameColumnMinWidth,
                    r.width - sizeColumnWidth - modifiedColumnPreferredWidth -
                        tableReserveWidth);

                if ( ImGui::BeginTable("FileTreeTableV2",
                                       3,
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
                        TR("ui.file_manager.column_size").data(),
                        ImGuiTableColumnFlags_WidthFixed |
                            ImGuiTableColumnFlags_PreferSortAscending,
                        sizeColumnWidth);
                    ImGui::TableSetupColumn(
                        TR("ui.file_manager.column_modified_time").data(),
                        ImGuiTableColumnFlags_WidthStretch |
                            ImGuiTableColumnFlags_PreferSortDescending,
                        1.0f);
                    ImGui::TableHeadersRow();
                    syncFileTableSortSpecs();

                    const float indent = ImGui::CalcTextSize("AA").x;
                    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, indent);
                    drawDirectoryRecursive(m_currentRoot, sourceManager);
                    ImGui::PopStyleVar();

                    renderFileSortContextMenu();
                    ImGui::EndTable();
                }
            });
    }

    CLayVBox rootVBox;
    rootVBox.setPadding(rootPadding, rootPadding, rootPadding, rootPadding)
        .setSpacing(compactGap)
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
        const float nameColumnWidth = ImGui::GetContentRegionAvail().x;
        const bool  open            = Utils::renderScrollingTreeNode(
            entry.fullPath,
            displayName,
            nameColumnWidth,
            itemH,
            !entry.isDirectory,
            [this, &entry, sourceManager]() {
                activateFileEntry(entry, sourceManager);
            },
            entry.fullPath);
        renderFileEntryContextMenu(entry, sourceManager);

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

void FileManagerView::syncFileTableSortSpecs()
{
    ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
    if ( !sortSpecs || sortSpecs->SpecsCount <= 0 || !sortSpecs->SpecsDirty ) {
        return;
    }

    const ImGuiTableColumnSortSpecs& primarySpec = sortSpecs->Specs[0];
    FileSortKey                      newSortKey  = FileSortKey::Name;
    if ( primarySpec.ColumnIndex == FileTableColumnSize ) {
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
    if ( !ImGui::BeginPopupContextWindow("FileTreeTableContextMenu",
                                         ImGuiPopupFlags_MouseButtonRight) ) {
        return;
    }

    if ( ImGui::MenuItem(TR("ui.file_manager.context.refresh").data()) ) {
        invalidateDirectoryCache();
    }

    bool directoriesFirst = m_directoriesFirst;
    if ( ImGui::MenuItem(TR("ui.file_manager.context.directories_first").data(),
                         nullptr,
                         directoriesFirst) ) {
        m_directoriesFirst = !m_directoriesFirst;
        invalidateDirectoryCache();
    }

    ImGui::EndPopup();
}

void FileManagerView::renderFileEntryContextMenu(
    const DirectoryEntryInfo& entry, UIManager* sourceManager)
{
    const std::string popupId = "FileEntryContext_" + entry.fullPath;
    if ( !ImGui::BeginPopupContextItem(popupId.c_str()) ) {
        return;
    }

    if ( !entry.isDirectory && (isBeatmapExtension(entry.extension) ||
                                isAudioExtension(entry.extension)) ) {
        if ( ImGui::MenuItem(TR("ui.file_manager.context.open").data()) ) {
            activateFileEntry(entry, sourceManager);
        }
    }

    if ( entry.isDirectory &&
         ImGui::MenuItem(TR("ui.file_manager.context.refresh").data()) ) {
        m_directoryCache.erase(directoryCacheKey(entry.path));
    }

    if ( ImGui::MenuItem(TR("ui.file_manager.context.copy_path").data()) ) {
        ImGui::SetClipboardText(entry.fullPath.c_str());
    }

    ImGui::EndPopup();
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
