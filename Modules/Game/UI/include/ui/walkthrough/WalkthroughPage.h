#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace MMM::UI
{
class UIManager;
/// @brief 欢迎页内嵌的演练正文，不创建独立窗口，也不持有学习进度。
class WalkthroughPage
{
public:
    /// @brief 在欢迎页正文区域绘制指定主题和可独立展开的操作分支。
    /// @warning UI 可见帧只读取目录与进度，文件访问仅来自显式操作或进度变更。
    void render(UIManager* manager, std::size_t topicIndex);

private:
    std::string m_currentTopic;  ///< 当前正文主题，切换主题时重置展开项。
    std::optional<std::size_t> m_expandedBranch{ 0 };  ///< 只展开一个操作分支。
    std::string                m_preparedTopic;  ///< 已提交图片准备的主题 ID。
    std::string                m_preparedLanguage;  ///< 图片准备时的语言。
};
}  // namespace MMM::UI
