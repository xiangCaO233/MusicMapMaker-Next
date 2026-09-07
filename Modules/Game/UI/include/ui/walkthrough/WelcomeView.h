#pragma once

#include "ui/IUIView.h"
#include "ui/walkthrough/WalkthroughPage.h"
#include <cstddef>
#include <optional>
#include <string>

namespace MMM::UI
{
/// @brief 独立的欢迎标签页，主题目录和演练正文共享窗口与导航生命周期。
class WelcomeView final : public IUIView
{
public:
    /// @brief 创建欢迎页，首次显示时停靠到主工作区。
    WelcomeView();
    /// @brief 返回主题目录并请求焦点，不清空学习进度。
    void showHome();
    /// @brief 布局替换前保留停靠意图；显式浮动的欢迎页不参与重新停靠。
    /// @warning 仅项目布局恢复或停靠树重建前调用，不重置导航、焦点或进度。
    void prepareForDockLayoutChange();
    /// @brief 在同一欢迎标签页进入主题，保留服务中的学习进度。
    void showTopic(std::size_t topicIndex);
    /// @brief 查询当前是否显示主题目录。
    bool showingHome() const { return !m_topic.has_value(); }
    /// @brief 绘制主题目录、内嵌演练和固定页脚。
    /// @warning 每个可见帧调用；禁止磁盘扫描，配置写入只响应用户切换选项。
    void update(UIManager* manager) override;

private:
    /// @brief 绘制跟随皮肤配色的主题卡片目录。
    /// @warning 可见首页每帧调用，仅访问已加载主题和进度。
    void                       renderHome(UIManager* manager);
    std::optional<std::size_t> m_topic;  ///< 当前主题；空值表示欢迎首页。
    std::string m_chapter;  ///< 当前章节稳定 ID，返回目录和切换语言时保留。
    bool        m_restoreChapter{ true };  ///< 返回目录后仅恢复一次章节选择。
    WalkthroughPage m_walkthrough;         ///< 内嵌正文，不拥有窗口和学习记录。
    bool            m_focus{ true };       ///< 显式打开后请求一次窗口焦点。
    bool            m_dockToCenter{ true };  ///< 等待主停靠区域可用后停靠一次。
    bool m_scrollToTop{ true };  ///< 页面切换后重置正文滚动，不影响进度。
    bool m_saveFailed{ false };  ///< 最近一次欢迎页设置保存失败。
};
}  // namespace MMM::UI
