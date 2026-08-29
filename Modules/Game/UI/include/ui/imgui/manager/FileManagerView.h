#pragma once

#include "event/core/EventBus.h"
#include "ui/ISubView.h"
#include "ui/layout/box/CLayBox.h"
#include <array>
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

    /// @brief 同步项目活动状态和文件树根目录快照。
    /// @param sourceManager 当前 UI 管理器。
    /// @warning UI 热路径：只比较路径并在发生变化时使目录缓存失效。
    void syncProjectUiState(UIManager* sourceManager) override;

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
    /// @brief 文件管理器剪贴板操作模式。
    enum class FileClipboardMode {
        /// @brief 当前没有可粘贴项目。
        None,

        /// @brief 复制源文件或目录。
        Copy,

        /// @brief 移动源文件或目录。
        Cut
    };

    /// @brief 空项目占位布局的运行时尺寸缓存。
    struct EmptyProjectViewMetrics {
        /// @brief 占位内容外侧留白。
        float padding{ 0.0f };

        /// @brief 占位行之间的纵向间距。
        float gap{ 0.0f };

        /// @brief 初始提示行保留高度。
        float hintRowHeight{ 0.0f };

        /// @brief 打开目录按钮行保留高度。
        float buttonRowHeight{ 0.0f };

        /// @brief 最近项目区标题保留高度。
        float recentTitleHeight{ 0.0f };

        /// @brief 每个最近项目条目保留高度。
        float recentItemHeight{ 0.0f };

        /// @brief 最近项目列表前的顶部留白。
        float recentTopPadding{ 0.0f };

        /// @brief 实际打开目录按钮高度。
        float buttonHeight{ 0.0f };
    };

    /// @brief 计算感知字体尺寸的占位布局指标。
    /// @param dpiScale 当前窗口内容缩放。
    /// @return 适配当前 ImGui 字体和 DPI 缩放的尺寸指标。
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

    /// @brief 绘制文件树空白区域右键菜单。
    /// @param sourceManager 打开新建谱面向导所需的 UI 管理器。
    void renderFileBackgroundContextMenu(UIManager* sourceManager);

    /// @brief 绘制文件操作弹窗。
    /// @param dpiScale 当前窗口内容缩放。
    void renderFileOperationPopups(float dpiScale);

    /// @brief 根据 ImGui 表格排序状态同步文件树排序设置。
    void syncFileTableSortSpecs();

    /// @brief 绘制单个文件或目录的右键菜单。
    /// @param entry 当前条目快照。
    /// @param sourceManager 触发文件打开后需要切换子视图的 UI 管理器。
    void renderFileEntryContextMenu(const DirectoryEntryInfo& entry,
                                    UIManager*                sourceManager);

    /// @brief 打开新建谱面向导。
    /// @param sourceManager 当前 UI 管理器。
    void openNewBeatmapWizard(UIManager* sourceManager);

    /// @brief 请求重命名指定文件或目录。
    /// @param path 需要重命名的路径。
    void requestRename(const std::filesystem::path& path);

    /// @brief 请求在指定目录中新建文件夹。
    /// @param directory 新文件夹所在目录。
    void requestNewFolder(const std::filesystem::path& directory);

    /// @brief 请求删除指定文件或目录。
    /// @param path 需要删除的路径。
    void requestDelete(const std::filesystem::path& path);

    /// @brief 将指定文件或目录写入内部剪贴板。
    /// @param path 剪贴板来源路径。
    /// @param cut 是否为剪切模式。
    void setFileClipboard(const std::filesystem::path& path, bool cut);

    /// @brief 判断当前文件剪贴板是否可粘贴。
    /// @return 源路径仍然存在时返回 true。
    bool hasPasteableFileClipboard() const;

    /// @brief 将内部文件剪贴板粘贴到目标目录。
    /// @param targetDirectory 目标目录。
    void pasteFileClipboardInto(const std::filesystem::path& targetDirectory);

    /// @brief 执行当前待确认的重命名操作。
    void confirmRename();

    /// @brief 执行当前待确认的新建文件夹操作。
    void confirmNewFolder();

    /// @brief 执行当前待确认的删除操作。
    void confirmDelete();

    /// @brief 将文本安全写入文件操作输入框。
    /// @param value 输入文本。
    void setFileOperationInput(const std::string& value);

    /// @brief 执行文件树条目的默认打开动作。
    /// @param entry 当前条目快照。
    /// @param sourceManager 触发文件打开后需要切换子视图的 UI 管理器。
    void activateFileEntry(const DirectoryEntryInfo& entry,
                           UIManager*                sourceManager);

    void openFolderPicker();

    /// @brief UI 生命周期快照是否确认当前已有加载完成的项目。
    bool m_hasActiveProjectUiState{ false };

    /// @brief 当前项目文件树使用的根目录快照。
    std::filesystem::path m_currentRoot;
    bool                  m_showRoot = true;
    /// @brief 当前文件树排序字段。
    FileSortKey m_fileSortKey{ FileSortKey::Name };

    /// @brief 当前文件树排序方向。
    SortDirection m_fileSortDirection{ SortDirection::Ascending };

    /// @brief 文件树是否始终将目录排在普通文件前。
    bool m_directoriesFirst{ true };

    /// @brief 文件操作输入框缓冲区。
    std::array<char, 256> m_fileOperationInput{};

    /// @brief 最近一次文件操作失败信息。
    std::string m_fileOperationError;

    /// @brief 重命名目标路径。
    std::filesystem::path m_pendingRenamePath;

    /// @brief 新建文件夹目标目录。
    std::filesystem::path m_pendingNewFolderDirectory;

    /// @brief 删除目标路径。
    std::filesystem::path m_pendingDeletePath;

    /// @brief 下一帧是否打开重命名弹窗。
    bool m_shouldOpenRenamePopup{ false };

    /// @brief 下一帧是否打开新建文件夹弹窗。
    bool m_shouldOpenNewFolderPopup{ false };

    /// @brief 下一帧是否打开删除确认弹窗。
    bool m_shouldOpenDeletePopup{ false };

    /// @brief 弹窗打开后是否需要聚焦文件名输入框。
    bool m_shouldFocusFileOperationInput{ false };

    /// @brief 内部文件剪贴板来源路径。
    std::filesystem::path m_fileClipboardPath;

    /// @brief 内部文件剪贴板模式。
    FileClipboardMode m_fileClipboardMode{ FileClipboardMode::None };

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

    /// @brief 项目目录重扫事件订阅 ID。
    Event::SubscriptionID m_projectDirectoryRefreshedSubId{ 0 };
};

}  // namespace MMM::UI
