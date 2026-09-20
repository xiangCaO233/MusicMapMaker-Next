#pragma once

#include <cstddef>
#include <cstdint>
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
    /// @param manager 提供演练服务、图片缓存和视图注册表的 UI 管理器。
    /// @param topicIndex 当前目录选中的主题索引。
    /// @warning UI 可见帧只读取目录与进度，文件访问仅来自显式操作或进度变更。
    void render(UIManager* manager, std::size_t topicIndex);

private:
    /// @brief 当前由用户启动的路线引导身份及当前步骤状态。
    struct ActiveGuide {
        /// @brief 主题稳定 ID。
        std::string topicId;
        /// @brief 分支稳定 ID。
        std::string branchId;
        /// @brief 当前步骤稳定 ID。
        std::string stepId;
        /// @brief 当前步骤启动时已观察到的业务信号序号。
        std::uint64_t signalRevisionAtStart{ 0 };
    };

    /// @brief 当前正文主题 ID，切换主题时用于重置展开项。
    std::string m_currentTopic;

    /// @brief 当前唯一展开的操作分支；空值表示全部折叠。
    std::optional<std::size_t> m_expandedBranch{ 0 };

    /// @brief 最近已提交图片准备的主题 ID。
    std::string m_preparedTopic;

    /// @brief 图片准备时采用的语言，用于检测本地化切换。
    std::string m_preparedLanguage;

    /// @brief 当前突出引导；页面隐藏时不续租，因此不会残留全屏遮罩。
    std::optional<ActiveGuide> m_activeGuide;

    /// @brief 上次正文实际渲染的 ImGui 帧，用于返回页面时清除旧引导。
    int m_lastRenderFrame{ -1 };
};
}  // namespace MMM::UI
