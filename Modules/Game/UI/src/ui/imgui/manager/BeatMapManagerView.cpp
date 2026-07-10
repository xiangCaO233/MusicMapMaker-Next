#ifndef IMGUI_DEFINE_MATH_OPERATORS
#    define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "ui/imgui/manager/BeatMapManagerView.h"
#include "config/Utf8Path.h"
#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "imgui.h"
#include "logic/EditorEngine.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/project/Project.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/layout/box/CLayBox.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fmt/format.h>
#include <imgui_internal.h>
#include <numeric>
#include <system_error>

namespace MMM::UI
{
namespace
{
/// @brief 谱面表格列编号。
enum BeatmapTableColumn : int {
    /// @brief 名称列。
    BeatmapTableColumnName = 0,

    /// @brief 类型列。
    BeatmapTableColumnType = 1,

    /// @brief 谱面版本列。
    BeatmapTableColumnVersion = 2,

    /// @brief 文件路径列。
    BeatmapTableColumnPath = 3,

    /// @brief 文件大小列。
    BeatmapTableColumnSize = 4,

    /// @brief 修改时间列。
    BeatmapTableColumnModifiedTime = 5
};

/// @brief 将 ASCII 字符串转换为小写，用于类型归一化和排序。
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

/// @brief 根据谱面路径推导显示类型。
/// @param filePath 项目内谱面相对路径。
/// @return 面向谱面管理器的短类型文本。
std::string beatmapTypeFromPath(const std::string& filePath)
{
    auto extension = toLowerAscii(
        Config::pathToUtf8(Config::utf8ToPath(filePath).extension()));
    if ( extension == ".osu" ) {
        return "osu";
    }
    if ( extension == ".mc" ) {
        return "malody";
    }
    if ( extension == ".imd" ) {
        return "rm";
    }
    if ( extension == ".mmm" ) {
        return "mmm";
    }
    if ( extension.size() > 1 && extension.front() == '.' ) {
        extension.erase(extension.begin());
    }
    return extension.empty() ? TR("ui.file_manager.value_unknown").data()
                             : extension;
}

/// @brief 读取文件大小和修改时间。
/// @param filePath 需要查询的绝对文件路径。
/// @return 文件系统元数据；读取失败的字段保持无效。
BeatMapManagerView::FileMetadata queryBeatmapFileMetadata(
    const std::filesystem::path& filePath)
{
    BeatMapManagerView::FileMetadata metadata;

    std::error_code filesystemError;
    const auto size = std::filesystem::file_size(filePath, filesystemError);
    if ( !filesystemError ) {
        metadata.size    = size;
        metadata.hasSize = true;
    }

    filesystemError.clear();
    const auto modifiedTime =
        std::filesystem::last_write_time(filePath, filesystemError);
    if ( !filesystemError ) {
        metadata.lastWriteTime    = modifiedTime;
        metadata.hasLastWriteTime = true;
    }
    return metadata;
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

/// @brief 生成文件大小列文本。
/// @param metadata 文件元数据缓存。
/// @return 大小或未知占位。
std::string formatSizeColumn(const BeatMapManagerView::FileMetadata& metadata)
{
    if ( !metadata.hasSize ) {
        return TR("ui.file_manager.value_unknown").data();
    }
    return formatFileSize(metadata.size);
}

/// @brief 生成修改时间列文本。
/// @param metadata 文件元数据缓存。
/// @return 修改时间或未知占位。
std::string formatModifiedColumn(
    const BeatMapManagerView::FileMetadata& metadata)
{
    if ( !metadata.hasLastWriteTime ) {
        return TR("ui.file_manager.value_unknown").data();
    }
    return formatModifiedTime(metadata.lastWriteTime);
}

/// @brief 生成谱面 Version 列文本。
/// @param metadata 谱面元数据缓存。
/// @return Version 字段文本或未知占位。
std::string formatVersionColumn(
    const BeatMapManagerView::FileMetadata& metadata)
{
    if ( !metadata.hasVersion ) {
        return TR("ui.file_manager.value_unknown").data();
    }
    return metadata.version;
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

/// @brief 组合排序菜单项显示文本。
/// @param columnLabel 排序字段显示名。
/// @param ascending 是否升序。
/// @return 带方向后缀的菜单文本。
std::string makeSortMenuLabel(const char* columnLabel, bool ascending)
{
    return ascending
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

/// @brief 绘制可裁剪、超宽自动滚动的表格单元格文本。
/// @param text 需要绘制的文本。
/// @param cursorPos 单元格起始屏幕坐标。
/// @param width 单元格可用宽度。
/// @param height 行高。
void renderScrollingTableText(const std::string& text, ImVec2 cursorPos,
                              float width, float height)
{
    const float  textWidth = std::max(0.0f, width);
    const ImVec2 textSize  = ImGui::CalcTextSize(text.c_str());
    const float  textH     = ImGui::GetFontSize();
    const float  offsetY   = (height - textH) * 0.5f;

    float offset = 0.0f;
    if ( textSize.x > textWidth ) {
        const float scrollRange = textSize.x - textWidth + 40.0f;
        const float time        = static_cast<float>(ImGui::GetTime());
        float       t           = sinf(time * 0.5f - 1.57f) * 0.5f + 0.5f;
        t                       = std::clamp((t - 0.1f) / 0.8f, 0.0f, 1.0f);
        offset                  = t * scrollRange;
    }

    const ImVec2 textStartPos = cursorPos;
    ImGui::PushClipRect(
        textStartPos,
        ImVec2(textStartPos.x + textWidth, textStartPos.y + height),
        true);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(textStartPos.x - offset, textStartPos.y + offsetY),
        ImGui::GetColorU32(ImGuiCol_Text),
        text.c_str());
    ImGui::PopClipRect();
}

}  // namespace

/// @brief 获取谱面管理器中不可再换行控件所需的最小内容尺寸。
/// @warning UI 热路径：子视图可见时每帧查询；仅保留轻量文本测量。
ImVec2 BeatMapManagerView::getMinContentSize(float dpiScale) const
{
    auto&       engine    = Logic::EditorEngine::instance();
    auto*       project   = engine.getCurrentProject();
    const float scale     = std::max(1.0f, dpiScale);
    const float panelPad  = 4.0f * scale;
    const float rowHeight = 28.0f * scale;
    const float footerH   = 44.0f * scale;
    const float headerWidth =
        ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x +
        ImGui::CalcTextSize(TR("ui.beatmap_manager.beatmaps").data()).x;

    if ( !project ) {
        const char* hint = TR("ui.beatmap_manager.initial_hint").data();
        return ImVec2(
            std::ceil(ImGui::CalcTextSize(hint).x + panelPad * 2.0f),
            std::ceil(ImGui::GetTextLineHeightWithSpacing() + panelPad * 2.0f));
    }

    const float visibleRows =
        m_showBeatmapList ? std::min<size_t>(project->m_beatmaps.size(), 1) : 0;
    const float minHeight = panelPad * 2.0f + ImGui::GetFrameHeight() +
                            visibleRows * rowHeight + footerH;
    return ImVec2(std::ceil(headerWidth + panelPad * 2.0f),
                  std::ceil(minHeight));
}

void BeatMapManagerView::onUpdate(LayoutContext& layoutContext,
                                  UIManager*     sourceManager)
{
    auto& engine  = Logic::EditorEngine::instance();
    auto* project = engine.getCurrentProject();
    auto& skinCfg = Config::SkinManager::instance();

    float   dpiScale        = layoutContext.m_dpiScale;
    ImFont* fileManagerFont = skinCfg.getFont("filemanager");
    if ( fileManagerFont ) {
        ImGui::PushFont(fileManagerFont, fileManagerFont->LegacySize);
    }

    const float panelPadding = std::min(
        4.0f * dpiScale, std::max(0.0f, layoutContext.m_avail.x) * 0.02f);
    const float rowHeight = 28.0f * dpiScale;

    if ( !project ) {
        const char* hint     = TR("ui.beatmap_manager.initial_hint").data();
        ImVec2      textSize = ImGui::CalcTextSize(hint);
        ImVec2      textPos  = {
            layoutContext.m_startPos.x +
                std::max(0.0f, layoutContext.m_avail.x - textSize.x) * 0.5f,
            layoutContext.m_startPos.y +
                std::max(0.0f, layoutContext.m_avail.y - textSize.y) * 0.5f
        };
        ImGui::SetCursorScreenPos(textPos);
        ImGui::TextDisabled("%s", hint);

        if ( fileManagerFont ) ImGui::PopFont();
        return;
    }

    float  footerH  = 44.0f * dpiScale;
    float  listH    = std::max(0.0f, layoutContext.m_avail.y - footerH);
    ImVec2 listPos  = { layoutContext.m_startPos.x + panelPadding,
                        layoutContext.m_startPos.y + panelPadding };
    ImVec2 listSize = { std::max(0.0f,
                                 layoutContext.m_avail.x - panelPadding * 2.0f),
                        std::max(0.0f, listH - panelPadding) };

    ImGui::SetCursorScreenPos(listPos);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild(
        "BeatmapListChild", listSize, false, ImGuiWindowFlags_None);

    ImVec2           headerPos = ImGui::GetCursorScreenPos();
    Clay_BoundingBox headerBox{ .x      = headerPos.x,
                                .y      = headerPos.y,
                                .width  = ImGui::GetContentRegionAvail().x,
                                .height = ImGui::GetFrameHeight() };
    Utils::renderCollapsingHeader(TR("ui.beatmap_manager.beatmaps").data(),
                                  &m_showBeatmapList,
                                  headerBox);

    if ( m_showBeatmapList ) {
        Utils::VerticalScrollbarStyleScope verticalScrollbarStyle(dpiScale);
        const ImGuiTableFlags              tableFlags =
            ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable |
            ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_SizingStretchProp;

        if ( ImGui::BeginTable("BeatmapManagerTableV2",
                               6,
                               tableFlags,
                               ImGui::GetContentRegionAvail()) ) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_name").data(),
                ImGuiTableColumnFlags_DefaultSort |
                    ImGuiTableColumnFlags_WidthStretch |
                    ImGuiTableColumnFlags_PreferSortAscending);
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_type").data(),
                ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_PreferSortAscending,
                std::max(76.0f, 86.0f * ImGui::GetFontSize() / 17.0f));
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_version").data(),
                ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_PreferSortAscending,
                std::max(96.0f, 120.0f * ImGui::GetFontSize() / 17.0f));
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_path").data(),
                ImGuiTableColumnFlags_DefaultHide |
                    ImGuiTableColumnFlags_WidthStretch |
                    ImGuiTableColumnFlags_PreferSortAscending);
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_size").data(),
                ImGuiTableColumnFlags_DefaultHide |
                    ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_PreferSortAscending,
                std::max(96.0f, 104.0f * ImGui::GetFontSize() / 17.0f));
            ImGui::TableSetupColumn(
                TR("ui.beatmap_manager.column_modified_time").data(),
                ImGuiTableColumnFlags_DefaultHide |
                    ImGuiTableColumnFlags_WidthFixed |
                    ImGuiTableColumnFlags_PreferSortDescending,
                std::max(168.0f,
                         ImGui::CalcTextSize("0000/00/00 00:00").x +
                             ImGui::GetStyle().CellPadding.x * 4.0f +
                             ImGui::GetFrameHeight()));
            if ( ImGuiTable* table = ImGui::GetCurrentTable() ) {
                table->DisableDefaultContextMenu = true;
            }
            ImGui::TableHeadersRow();

            ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            if ( sortSpecs && sortSpecs->SpecsCount > 0 &&
                 sortSpecs->SpecsDirty ) {
                const ImGuiTableColumnSortSpecs& primarySpec =
                    sortSpecs->Specs[0];
                BeatmapSortKey newSortKey = BeatmapSortKey::Name;
                if ( primarySpec.ColumnIndex == BeatmapTableColumnType ) {
                    newSortKey = BeatmapSortKey::Type;
                } else if ( primarySpec.ColumnIndex ==
                            BeatmapTableColumnVersion ) {
                    newSortKey = BeatmapSortKey::Version;
                } else if ( primarySpec.ColumnIndex ==
                            BeatmapTableColumnPath ) {
                    newSortKey = BeatmapSortKey::Path;
                } else if ( primarySpec.ColumnIndex ==
                            BeatmapTableColumnSize ) {
                    newSortKey = BeatmapSortKey::Size;
                } else if ( primarySpec.ColumnIndex ==
                            BeatmapTableColumnModifiedTime ) {
                    newSortKey = BeatmapSortKey::ModifiedTime;
                }

                const SortDirection newDirection =
                    primarySpec.SortDirection == ImGuiSortDirection_Descending
                        ? SortDirection::Descending
                        : SortDirection::Ascending;
                if ( newSortKey != m_beatmapSortKey ||
                     newDirection != m_beatmapSortDirection ) {
                    m_beatmapSortKey        = newSortKey;
                    m_beatmapSortDirection  = newDirection;
                    m_beatmapSortCacheDirty = true;
                }
                sortSpecs->SpecsDirty = false;
            }

            auto resetBeatmapTableSort = [&]() {
                if ( m_beatmapSortKey != BeatmapSortKey::Name ||
                     m_beatmapSortDirection != SortDirection::Ascending ) {
                    m_beatmapSortKey        = BeatmapSortKey::Name;
                    m_beatmapSortDirection  = SortDirection::Ascending;
                    m_beatmapSortCacheDirty = true;
                }
            };

            auto applyBeatmapSort = [&](BeatmapSortKey sortKey,
                                        SortDirection  direction) {
                if ( m_beatmapSortKey != sortKey ||
                     m_beatmapSortDirection != direction ) {
                    m_beatmapSortKey        = sortKey;
                    m_beatmapSortDirection  = direction;
                    m_beatmapSortCacheDirty = true;
                }
            };

            auto renderBeatmapTableHeaderContextMenu = [&]() {
                ImGuiTable* table = ImGui::GetCurrentTable();
                if ( !table ) return;

                ImGuiStyle&  style = ImGui::GetStyle();
                const ImVec2 popupPadding(
                    std::max(style.WindowPadding.x, 8.0f),
                    std::max(style.WindowPadding.y, 6.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, popupPadding);
                ImGui::PushStyleVar(
                    ImGuiStyleVar_ItemSpacing,
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
                if ( contextColumn >= 0 &&
                     isTableColumnEnabled(table, contextColumn) &&
                     ::MMM::UI::FeedbackMenuItem(
                         TR("ui.beatmap_manager.table_menu.size_column_fit")
                             .data()) ) {
                    ImGui::TableSetColumnWidthAutoSingle(table, contextColumn);
                }

                if ( ::MMM::UI::FeedbackMenuItem(
                         TR("ui.beatmap_manager.table_menu.size_all_default")
                             .data()) ) {
                    ImGui::TableSetColumnWidthAutoAll(table);
                }

                auto sortMenuItem = [&](BeatmapSortKey sortKey,
                                        SortDirection  direction,
                                        const char*    columnLabel) {
                    const std::string label = makeSortMenuLabel(
                        columnLabel, direction == SortDirection::Ascending);
                    const bool selected = m_beatmapSortKey == sortKey &&
                                          m_beatmapSortDirection == direction;
                    if ( ::MMM::UI::FeedbackMenuItem(
                             label.c_str(), nullptr, selected) ) {
                        applyBeatmapSort(sortKey, direction);
                    }
                };
                if ( ::MMM::UI::FeedbackBeginMenu(
                         TR("ui.resource_table.sort").data()) ) {
                    sortMenuItem(BeatmapSortKey::Name,
                                 SortDirection::Ascending,
                                 TR("ui.beatmap_manager.column_name").data());
                    sortMenuItem(BeatmapSortKey::Name,
                                 SortDirection::Descending,
                                 TR("ui.beatmap_manager.column_name").data());
                    sortMenuItem(BeatmapSortKey::Type,
                                 SortDirection::Ascending,
                                 TR("ui.beatmap_manager.column_type").data());
                    sortMenuItem(BeatmapSortKey::Type,
                                 SortDirection::Descending,
                                 TR("ui.beatmap_manager.column_type").data());
                    sortMenuItem(
                        BeatmapSortKey::Version,
                        SortDirection::Ascending,
                        TR("ui.beatmap_manager.column_version").data());
                    sortMenuItem(
                        BeatmapSortKey::Version,
                        SortDirection::Descending,
                        TR("ui.beatmap_manager.column_version").data());
                    sortMenuItem(BeatmapSortKey::Path,
                                 SortDirection::Ascending,
                                 TR("ui.beatmap_manager.column_path").data());
                    sortMenuItem(BeatmapSortKey::Path,
                                 SortDirection::Descending,
                                 TR("ui.beatmap_manager.column_path").data());
                    sortMenuItem(BeatmapSortKey::Size,
                                 SortDirection::Ascending,
                                 TR("ui.beatmap_manager.column_size").data());
                    sortMenuItem(BeatmapSortKey::Size,
                                 SortDirection::Descending,
                                 TR("ui.beatmap_manager.column_size").data());
                    sortMenuItem(
                        BeatmapSortKey::ModifiedTime,
                        SortDirection::Ascending,
                        TR("ui.beatmap_manager.column_modified_time").data());
                    sortMenuItem(
                        BeatmapSortKey::ModifiedTime,
                        SortDirection::Descending,
                        TR("ui.beatmap_manager.column_modified_time").data());
                    ::MMM::UI::FeedbackEndMenu();
                }

                if ( ::MMM::UI::FeedbackBeginMenu(
                         TR("ui.beatmap_manager.table_menu.reset").data()) ) {
                    if ( ::MMM::UI::FeedbackMenuItem(
                             TR("ui.beatmap_manager.table_menu.reset_all")
                                 .data()) ) {
                        ImGui::TableResetSettings(table);
                        resetBeatmapTableSort();
                    }
                    if ( ::MMM::UI::FeedbackMenuItem(
                             TR("ui.beatmap_manager.table_menu.reset_columns")
                                 .data()) ) {
                        ImGui::TableSetColumnWidthAutoAll(table);
                    }
                    if ( ::MMM::UI::FeedbackMenuItem(
                             TR("ui.beatmap_manager.table_menu.show_all_"
                                "columns")
                                 .data()) ) {
                        for ( int column = 0; column < table->ColumnsCount;
                              ++column ) {
                            queueTableColumnEnabled(table, column, true);
                        }
                    }
                    if ( ::MMM::UI::FeedbackMenuItem(
                             TR("ui.beatmap_manager.table_menu.reset_sort")
                                 .data()) ) {
                        resetBeatmapTableSort();
                    }
                    ::MMM::UI::FeedbackEndMenu();
                }

                ImGui::Separator();

                const std::array<const char*, 6> columnLabels{
                    TR("ui.beatmap_manager.column_name").data(),
                    TR("ui.beatmap_manager.column_type").data(),
                    TR("ui.beatmap_manager.column_version").data(),
                    TR("ui.beatmap_manager.column_path").data(),
                    TR("ui.beatmap_manager.column_size").data(),
                    TR("ui.beatmap_manager.column_modified_time").data()
                };
                int enabledColumnCount = 0;
                for ( int column = 0; column < table->ColumnsCount; ++column ) {
                    if ( isTableColumnUserEnabled(table, column) ) {
                        enabledColumnCount++;
                    }
                }
                for ( int column = 0; column < table->ColumnsCount; ++column ) {
                    const bool enabled =
                        isTableColumnUserEnabled(table, column);
                    const bool canToggle = !enabled || enabledColumnCount > 1;
                    if ( ::MMM::UI::FeedbackMenuItem(columnLabels[column],
                                                     nullptr,
                                                     enabled,
                                                     canToggle) ) {
                        queueTableColumnEnabled(table, column, !enabled);
                    }
                }

                ImGui::EndPopup();
                ImGui::PopStyleVar(2);
            };
            renderBeatmapTableHeaderContextMenu();

            auto rebuildSortCache = [&]() {
                const auto& beatmaps = project->m_beatmaps;
                m_sortedBeatmapIndices.resize(beatmaps.size());
                m_beatmapFileMetadata.resize(beatmaps.size());
                std::iota(m_sortedBeatmapIndices.begin(),
                          m_sortedBeatmapIndices.end(),
                          size_t{ 0 });
                for ( size_t index = 0; index < beatmaps.size(); ++index ) {
                    const auto filePath =
                        project->m_projectRoot /
                        Config::utf8ToPath(beatmaps[index].m_filePath);
                    m_beatmapFileMetadata[index] =
                        queryBeatmapFileMetadata(filePath);
                    auto beatmap = MMM::BeatMap::loadFromFile(filePath);
                    if ( !beatmap.m_baseMapMetadata.map_path.empty() ) {
                        m_beatmapFileMetadata[index].version =
                            beatmap.m_baseMapMetadata.version;
                        m_beatmapFileMetadata[index].hasVersion = true;
                    }
                }
                std::stable_sort(
                    m_sortedBeatmapIndices.begin(),
                    m_sortedBeatmapIndices.end(),
                    [&](size_t lhsIndex, size_t rhsIndex) {
                        const auto& lhs           = beatmaps[lhsIndex];
                        const auto& rhs           = beatmaps[rhsIndex];
                        int         compareResult = 0;
                        switch ( m_beatmapSortKey ) {
                        case BeatmapSortKey::Name:
                            compareResult =
                                toLowerAscii(lhs.m_name)
                                    .compare(toLowerAscii(rhs.m_name));
                            break;
                        case BeatmapSortKey::Type:
                            compareResult = beatmapTypeFromPath(lhs.m_filePath)
                                                .compare(beatmapTypeFromPath(
                                                    rhs.m_filePath));
                            break;
                        case BeatmapSortKey::Version:
                            compareResult = compareOptionalValue(
                                toLowerAscii(
                                    m_beatmapFileMetadata[lhsIndex].version),
                                m_beatmapFileMetadata[lhsIndex].hasVersion,
                                toLowerAscii(
                                    m_beatmapFileMetadata[rhsIndex].version),
                                m_beatmapFileMetadata[rhsIndex].hasVersion);
                            break;
                        case BeatmapSortKey::Path:
                            compareResult =
                                toLowerAscii(lhs.m_filePath)
                                    .compare(toLowerAscii(rhs.m_filePath));
                            break;
                        case BeatmapSortKey::Size:
                            compareResult = compareOptionalValue(
                                m_beatmapFileMetadata[lhsIndex].size,
                                m_beatmapFileMetadata[lhsIndex].hasSize,
                                m_beatmapFileMetadata[rhsIndex].size,
                                m_beatmapFileMetadata[rhsIndex].hasSize);
                            break;
                        case BeatmapSortKey::ModifiedTime:
                            compareResult = compareOptionalValue(
                                m_beatmapFileMetadata[lhsIndex].lastWriteTime,
                                m_beatmapFileMetadata[lhsIndex]
                                    .hasLastWriteTime,
                                m_beatmapFileMetadata[rhsIndex].lastWriteTime,
                                m_beatmapFileMetadata[rhsIndex]
                                    .hasLastWriteTime);
                            break;
                        }

                        if ( compareResult == 0 ) {
                            compareResult =
                                toLowerAscii(lhs.m_filePath)
                                    .compare(toLowerAscii(rhs.m_filePath));
                        }
                        if ( m_beatmapSortDirection ==
                             SortDirection::Descending ) {
                            compareResult = -compareResult;
                        }
                        return compareResult < 0;
                    });
                m_cachedBeatmapCount    = beatmaps.size();
                m_cachedBeatmapProject  = project;
                m_beatmapSortCacheDirty = false;
            };

            if ( m_beatmapSortCacheDirty ||
                 m_cachedBeatmapCount != project->m_beatmaps.size() ||
                 m_cachedBeatmapProject != project ) {
                rebuildSortCache();
            }

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_sortedBeatmapIndices.size()),
                          rowHeight);
            while ( clipper.Step() ) {
                for ( int row = clipper.DisplayStart; row < clipper.DisplayEnd;
                      ++row ) {
                    const size_t beatmapIndex =
                        m_sortedBeatmapIndices[static_cast<size_t>(row)];
                    if ( beatmapIndex >= project->m_beatmaps.size() ) {
                        continue;
                    }

                    const auto& beatmap = project->m_beatmaps[beatmapIndex];
                    const auto  typeText =
                        beatmapTypeFromPath(beatmap.m_filePath);
                    FileMetadata metadata;
                    if ( beatmapIndex < m_beatmapFileMetadata.size() ) {
                        metadata = m_beatmapFileMetadata[beatmapIndex];
                    }

                    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                    ImGui::TableNextColumn();

                    const ImVec2 nameCellPos = ImGui::GetCursorScreenPos();
                    const float  nameCellWidth =
                        ImGui::GetContentRegionAvail().x;
                    const std::string rowId =
                        "##BeatmapRow_" + beatmap.m_filePath;
                    const bool clicked = ::MMM::UI::FeedbackSelectable(
                        rowId.c_str(),
                        false,
                        ImGuiSelectableFlags_SpanAllColumns,
                        { 0.0f, rowHeight });
                    const bool hovered = ImGui::IsItemHovered();
                    if ( clicked ) {
                        XINFO("Request to load beatmap: {}", beatmap.m_name);
                        auto fullPath = project->m_projectRoot /
                                        Config::utf8ToPath(beatmap.m_filePath);
                        auto loadedBeatmap = std::make_shared<MMM::BeatMap>(
                            MMM::BeatMap::loadFromFile(fullPath));
                        engine.createSession(loadedBeatmap, beatmap.m_name);
                    }

                    if ( hovered &&
                         ImGui::IsMouseClicked(ImGuiMouseButton_Right) ) {
                        m_manageBeatmapPath = beatmap.m_filePath;
                        m_openManageModal   = true;
                    }
                    if ( hovered ) {
                        const std::string tooltipText =
                            fmt::format("File: {}\nType: {}\nTrack: {}",
                                        beatmap.m_filePath,
                                        typeText,
                                        beatmap.m_audioTrackId);
                        Utils::renderTooltip(tooltipText.c_str());
                    }

                    const std::string nameText =
                        std::string(ICON_MMM_FILE) + "  " + beatmap.m_name;
                    renderScrollingTableText(
                        nameText, nameCellPos, nameCellWidth, rowHeight);

                    ImGui::TableNextColumn();
                    renderScrollingTableText(typeText,
                                             ImGui::GetCursorScreenPos(),
                                             ImGui::GetContentRegionAvail().x,
                                             rowHeight);

                    ImGui::TableNextColumn();
                    renderScrollingTableText(formatVersionColumn(metadata),
                                             ImGui::GetCursorScreenPos(),
                                             ImGui::GetContentRegionAvail().x,
                                             rowHeight);

                    ImGui::TableNextColumn();
                    renderScrollingTableText(beatmap.m_filePath,
                                             ImGui::GetCursorScreenPos(),
                                             ImGui::GetContentRegionAvail().x,
                                             rowHeight);

                    ImGui::TableNextColumn();
                    renderScrollingTableText(formatSizeColumn(metadata),
                                             ImGui::GetCursorScreenPos(),
                                             ImGui::GetContentRegionAvail().x,
                                             rowHeight);

                    ImGui::TableNextColumn();
                    renderScrollingTableText(formatModifiedColumn(metadata),
                                             ImGui::GetCursorScreenPos(),
                                             ImGui::GetContentRegionAvail().x,
                                             rowHeight);
                }
            }
            ImGui::EndTable();
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();

    float  btnSize    = 32.0f * dpiScale;
    ImVec2 footerPos  = { layoutContext.m_startPos.x + panelPadding,
                          layoutContext.m_startPos.y + listH };
    ImVec2 footerSize = {
        std::max(0.0f, layoutContext.m_avail.x - panelPadding * 2.0f), footerH
    };
    ImVec2 buttonPos = {
        footerPos.x, footerPos.y + std::max(0.0f, footerSize.y - btnSize) * 0.5f
    };

    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.1f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.2f));
    Utils::pushFixedButtonStyleVars();

    ImGui::SetCursorScreenPos(buttonPos);
    ImDrawList* dl    = ImGui::GetWindowDrawList();
    ImVec4      bgCol = ImGui::GetStyle().Colors[ImGuiCol_FrameBg];
    bgCol.w *= 0.5f;
    float rounding = ImGui::GetStyle().FrameRounding;
    if ( ImGui::IsMouseHoveringRect(
             buttonPos,
             { buttonPos.x + footerSize.x, buttonPos.y + btnSize }) ) {
        bgCol.w *= 1.5f;
    }

    dl->AddRectFilled(buttonPos,
                      { buttonPos.x + footerSize.x, buttonPos.y + btnSize },
                      ImGui::ColorConvertFloat4ToU32(bgCol),
                      rounding);

    if ( ::MMM::UI::FeedbackButton(ICON_MMM_PLUS,
                                   ImVec2(footerSize.x, btnSize)) ) {
        auto* wizard =
            sourceManager->getView<NewBeatmapWizard>("NewBeatmapWizard");
        if ( wizard ) wizard->open();
    }

    Utils::popFixedButtonStyleVars();
    ImGui::PopStyleColor(4);
    if ( ImGui::IsItemHovered() ) {
        ImGui::SetTooltip("%s", TR_CACHE("ui.file.new_map").data());
    }

    // --- 3. 谱面管理窗口 ---
    bool showBMModal = !m_manageBeatmapPath.empty();
    if ( showBMModal ) {
        std::string windowTitle =
            fmt::format("{} {}###BeatmapManageWindow",
                        TR("ui.beatmap_manager.manage_title").data(),
                        m_manageBeatmapPath);
        if ( m_openManageModal ) {
            ::MMM::UI::FeedbackOpenPopup(windowTitle.c_str());
            m_openManageModal = false;
        }
        Utils::CenteredModalPopupScope manageWindowScope(dpiScale);
        const bool                     manageWindowOpened =
            manageWindowScope.begin(windowTitle.c_str(),
                                    &showBMModal,
                                    ImGuiWindowFlags_NoCollapse,
                                    { 420 * dpiScale, 0.0f });
        bool closeManageModal = !showBMModal;
        if ( manageWindowOpened ) {
            // --- 使用 Clay 重构对话框内容 ---
            CLayVBox    modalLayout;
            const auto& modalStyle     = ImGui::GetStyle();
            auto        toLayoutPixels = [](float value) {
                return static_cast<uint16_t>(std::ceil(std::max(0.0f, value)));
            };
            float padding =
                std::max(16.0f * dpiScale, modalStyle.WindowPadding.x);
            const float modalGap =
                std::max(16.0f * dpiScale, modalStyle.ItemSpacing.y);
            const float buttonGap =
                std::max(12.0f * dpiScale, modalStyle.ItemSpacing.x);
            const float buttonH =
                std::max(32.0f * dpiScale, ImGui::GetFrameHeight());
            const float removeButtonW =
                std::max(140.0f * dpiScale,
                         ImGui::CalcTextSize(
                             TR("ui.beatmap_manager.remove_beatmap").data())
                                 .x +
                             modalStyle.FramePadding.x * 2.0f);
            const float cancelButtonW =
                std::max(100.0f * dpiScale,
                         ImGui::CalcTextSize(TR("ui.common.cancel").data()).x +
                             modalStyle.FramePadding.x * 2.0f);
            const uint16_t modalPaddingPx = toLayoutPixels(padding);
            modalLayout.setPadding(
                modalPaddingPx, modalPaddingPx, modalPaddingPx, modalPaddingPx);
            modalLayout.setSpacing(toLayoutPixels(modalGap));

            // 1. 标题与信息 (移除了冗余 Text)
            modalLayout.addElement(
                "ModalSep",
                Sizing::Grow(),
                Sizing::Fixed(1),
                [=, this](Clay_BoundingBox r, bool) {
                    ImGui::GetWindowDrawList()->AddLine(
                        { r.x, r.y },
                        { r.x + r.width, r.y },
                        ImGui::GetColorU32(ImGuiCol_Separator));
                });

            // 2. 操作按钮区
            CLayHBox btnRow;
            btnRow.setAlignment(Alignment::Center());
            btnRow.setSpacing(toLayoutPixels(buttonGap));

            btnRow.addElement(
                "RemoveBtn",
                Sizing::Fixed(removeButtonW),
                Sizing::Fixed(buttonH),
                [=](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    if ( ::MMM::UI::FeedbackButton(
                             TR("ui.beatmap_manager.remove_beatmap").data(),
                             { r.width, r.height }) ) {
                        ::MMM::UI::FeedbackOpenPopup("RemoveBeatmapConfirm");
                    }
                });

            btnRow.addElement(
                "CancelBtn",
                Sizing::Fixed(cancelButtonW),
                Sizing::Fixed(buttonH),
                [=, this, &closeManageModal](Clay_BoundingBox r, bool) {
                    ImGui::SetCursorScreenPos({ r.x, r.y });
                    if ( ::MMM::UI::FeedbackButton(
                             TR("ui.common.cancel").data(),
                             { r.width, r.height }) ) {
                        closeManageModal = true;
                    }
                });

            modalLayout.addLayout(
                "BtnRowLayout", btnRow, Sizing::Grow(), Sizing::Fixed(buttonH));

            // 渲染布局
            ImVec2 modalSize = modalLayout.renderInCurrent(
                ImGui::GetCursorScreenPos(),
                { ImGui::GetContentRegionAvail().x, 0 });
            ImGui::Dummy(modalSize);

            // --- 二次确认弹窗 ---
            {
                Utils::CenteredModalPopupScope removeModalScope(dpiScale);
                if ( removeModalScope.begin("RemoveBeatmapConfirm") ) {
                    ImGui::Text("%s",
                                TR("ui.beatmap_manager.remove_confirm").data());
                    ImGui::Spacing();
                    if ( ::MMM::UI::FeedbackButton(
                             TR("ui.common.confirm").data(),
                             { 100 * dpiScale, 0 }) ) {
                        engine.pushCommand(
                            Logic::CmdRemoveBeatmap{ m_manageBeatmapPath });
                        closeManageModal = true;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if ( ::MMM::UI::FeedbackButton(
                             TR("ui.common.cancel").data(),
                             { 100 * dpiScale, 0 }) ) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
            }

            if ( closeManageModal ) {
                showBMModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if ( !showBMModal ) {
            m_manageBeatmapPath.clear();
        }
    }

    if ( fileManagerFont ) ImGui::PopFont();
}

}  // namespace MMM::UI
