#pragma once

#include "event/core/EventBus.h"
#include "ui/ISubView.h"
#include "ui/layout/box/CLayBox.h"
#include <deque>
#include <filesystem>
#include <glm/glm.hpp>
#include <string>
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
    FileManagerView(FileManagerView&&)                 = default;
    FileManagerView(const FileManagerView&)            = default;
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
    void renderActiveProjectView(LayoutContext& layoutContext,
                                 UIManager*     sourceManager);

    void drawDirectoryRecursive(const std::filesystem::path& path,
                                UIManager*                   sourceManager);
    void openFolderPicker();

    std::filesystem::path m_currentRoot;
    bool                  m_showRoot = true;

    // --- 布局池 ---
    std::deque<CLayHBox> m_rows;

    struct PendingDrop {
        std::vector<std::string> paths;
        glm::vec2                pos;
    };
    std::vector<PendingDrop> m_pendingDrops;
    Event::SubscriptionID    m_dropSubId;
};

}  // namespace MMM::UI
