#pragma once

#include "event/core/EventBus.h"
#include "ui/ISubView.h"
#include "ui/layout/box/CLayBox.h"
#include <concurrentqueue.h>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace MMM::Event
{
struct GLFWDropEvent;
}

namespace MMM::UI
{

class FileManagerView : public ISubView
{
public:
    FileManagerView(const std::string& subViewName);
    FileManagerView(FileManagerView&&)                 = delete;
    FileManagerView(const FileManagerView&)            = delete;
    FileManagerView& operator=(FileManagerView&&)      = delete;
    FileManagerView& operator=(const FileManagerView&) = delete;
    ~FileManagerView() override;

    // 内部绘制逻辑 (Clay/ImGui)
    void onUpdate(LayoutContext& layoutContext,
                  UIManager*     sourceManager) override;

    /// @brief 获取文件管理器中不可再换行控件所需的最小内容尺寸。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 文件管理器最小内容尺寸。
    /// @warning UI 热路径：子视图可见时每帧查询；仅保留配置读取和轻量文本测量。
    ImVec2 getMinContentSize(float dpiScale) const override;

    /// @brief 文件树排序字段。
    enum class FileSortKey {
        /// @brief 按文件名排序。
        Name,

        /// @brief 按扩展名或目录类型排序。
        Type,

        /// @brief 按文件大小排序。
        Size,

        /// @brief 按最后修改时间排序。
        ModifiedTime
    };

    /// @brief 文件树排序方向。
    enum class SortDirection {
        /// @brief 从小到大排序。
        Ascending,

        /// @brief 从大到小排序。
        Descending
    };

    /// @brief 缓存后的单个文件系统条目。
    struct DirectoryEntryInfo {
        /// @brief 条目的绝对路径。
        std::filesystem::path path;

        /// @brief 用于显示和名称排序的 UTF-8 文件名。
        std::string filename;

        /// @brief 用于 Tooltip 和剪贴板的 UTF-8 完整路径。
        std::string fullPath;

        /// @brief 用于类型排序的 UTF-8 扩展名。
        std::string extension;

        /// @brief 当前条目是否为目录。
        bool isDirectory{ false };

        /// @brief 当前条目是否为普通文件。
        bool isRegularFile{ false };

        /// @brief 普通文件大小；目录或不可访问文件保持为 0。
        std::uintmax_t fileSize{ 0 };

        /// @brief 是否成功读取普通文件大小。
        bool hasFileSize{ false };

        /// @brief 目录直属子项目数量；非目录或读取失败时保持为 0。
        std::uintmax_t directoryChildCount{ 0 };

        /// @brief 是否成功读取直属子项目数量。
        bool hasDirectoryChildCount{ false };

        /// @brief 文件系统最后修改时间；读取失败时不参与精确排序。
        std::filesystem::file_time_type lastWriteTime{};

        /// @brief 是否成功读取最后修改时间。
        bool hasLastWriteTime{ false };
    };

    /// @brief 单个目录在当前排序设置下的快照。
    struct DirectorySnapshot {
        /// @brief 目录内可显示条目。
        std::vector<DirectoryEntryInfo> entries;

        /// @brief 快照构建时使用的排序字段。
        FileSortKey sortKey{ FileSortKey::Name };

        /// @brief 快照构建时使用的排序方向。
        SortDirection sortDirection{ SortDirection::Ascending };

        /// @brief 快照构建时是否启用目录优先。
        bool directoriesFirst{ true };
    };

private:
    /// @brief Runtime metrics for the empty-project placeholder layout.
    struct EmptyProjectViewMetrics {
        /// @brief Outer padding around the placeholder content.
        float padding{ 0.0f };

        /// @brief Vertical gap between placeholder rows.
        float gap{ 0.0f };

        /// @brief Height reserved for the initial hint row.
        float hintRowHeight{ 0.0f };

        /// @brief Height reserved for the open-directory button row.
        float buttonRowHeight{ 0.0f };

        /// @brief Height reserved for the recent-project section title.
        float recentTitleHeight{ 0.0f };

        /// @brief Height reserved for each recent-project item.
        float recentItemHeight{ 0.0f };

        /// @brief Top padding before the recent-project list.
        float recentTopPadding{ 0.0f };

        /// @brief Height used by the actual open-directory button.
        float buttonHeight{ 0.0f };
    };

    /// @brief Calculate font-aware placeholder layout metrics.
    /// @param dpiScale Current window content scale.
    /// @return Metrics sized for the current ImGui font and DPI scale.
    [[nodiscard]] EmptyProjectViewMetrics getEmptyProjectViewMetrics(
        float dpiScale) const;

    void handleDragDrop(UIManager* sourceManager);
    /// @brief 渲染未打开项目时的文件浏览器占位内容。
    /// @param layoutContext 当前 Clay/ImGui 布局上下文。
    /// @warning UI 热路径：未打开项目且子视图可见时每帧执行。
    /// 避免文件系统扫描或高开销所有权操作。
    void renderEmptyProjectView(LayoutContext& layoutContext);
    /// @brief 渲染已打开项目时的文件树内容。
    /// @param layoutContext 当前 Clay/ImGui 布局上下文。
    /// @param sourceManager 触发文件打开后需要切换子视图的 UI 管理器。
    /// @warning UI 热路径：项目文件浏览器可见时每帧执行；
    /// 避免额外增加文件系统遍历和所有权复制。
    void renderActiveProjectView(LayoutContext& layoutContext,
                                 UIManager*     sourceManager);

    /// @brief 递归绘制目录树节点。
    /// @param path 当前要绘制的目录路径。
    /// @param sourceManager 触发文件打开后需要切换子视图的 UI 管理器。
    /// @warning UI 热路径：当前实现会随文件树展开状态遍历目录；
    /// 后续应缓存目录快照。
    void drawDirectoryRecursive(const std::filesystem::path& path,
                                UIManager*                   sourceManager);
    /// @brief 获取指定目录的缓存快照，必要时低频重建。
    /// @param path 需要展示的目录路径。
    /// @return 当前排序设置下的目录快照。
    /// @warning UI 热路径：仅在缓存失效或首次展开目录时访问文件系统和排序。
    const DirectorySnapshot& getDirectorySnapshot(
        const std::filesystem::path& path);

    /// @brief 清空目录快照缓存。
    void invalidateDirectoryCache();

    /// @brief 消费跨线程文件系统变更通知并刷新目录缓存。
    /// @warning UI 热路径：每帧只清空无锁队列；仅在收到保存事件时清空快照缓存。
    void consumePendingDirectoryRefreshes();

    /// @brief 绘制文件树右键排序菜单。
    void renderFileSortContextMenu();

    /// @brief 根据 ImGui 表格排序状态同步文件树排序设置。
    void syncFileTableSortSpecs();

    /// @brief 绘制单个文件或目录的右键菜单。
    /// @param entry 当前条目快照。
    /// @param sourceManager 触发文件打开后需要切换子视图的 UI 管理器。
    void renderFileEntryContextMenu(const DirectoryEntryInfo& entry,
                                    UIManager*                sourceManager);

    /// @brief 执行文件树条目的默认打开动作。
    /// @param entry 当前条目快照。
    /// @param sourceManager 触发文件打开后需要切换子视图的 UI 管理器。
    void activateFileEntry(const DirectoryEntryInfo& entry,
                           UIManager*                sourceManager);

    void openFolderPicker();

    std::filesystem::path m_currentRoot;
    bool                  m_showRoot = true;
    /// @brief 当前文件树排序字段。
    FileSortKey m_fileSortKey{ FileSortKey::Name };

    /// @brief 当前文件树排序方向。
    SortDirection m_fileSortDirection{ SortDirection::Ascending };

    /// @brief 文件树是否始终将目录排在普通文件前。
    bool m_directoriesFirst{ true };

    /// @brief 按目录完整路径缓存的文件树快照。
    std::unordered_map<std::string, DirectorySnapshot> m_directoryCache;

    /// @brief 保存事件跨线程投递的目录刷新请求。
    moodycamel::ConcurrentQueue<bool> m_pendingDirectoryRefreshes;

    // --- 布局池 ---
    std::deque<CLayHBox> m_rows;

    struct PendingDrop {
        std::vector<std::string> paths;
        glm::vec2                pos;
    };
    std::vector<PendingDrop> m_pendingDrops;
    Event::SubscriptionID    m_dropSubId{ 0 };
    Event::SubscriptionID    m_saveResultSubId{ 0 };
    Event::SubscriptionID    m_projectSavedSubId{ 0 };
};

}  // namespace MMM::UI
