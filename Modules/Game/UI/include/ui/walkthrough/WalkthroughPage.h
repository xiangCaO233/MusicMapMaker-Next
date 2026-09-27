#pragma once

#include "ui/walkthrough/ComposeLessonCatalog.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace MMM::Logic
{
class BeatmapSession;
struct ComposeLessonCapture;
}  // namespace MMM::Logic
namespace MMM::Common::Render
{
class RenderSnapshotBuffer;
}
namespace MMM::UI
{
class UIManager;
namespace Walkthrough
{
struct Branch;
struct Step;
struct Topic;
}  // namespace Walkthrough
/// @brief 欢迎页内嵌的演练正文，不创建独立窗口，也不持有学习进度。
class WalkthroughPage
{
public:
    /// @brief 退出页面时撤销只属于本演练的项目路径限制。
    ~WalkthroughPage();
    /// @brief 每帧推进当前路线，即使欢迎标签被其它 Dock 标签遮住也继续运行。
    /// @param manager 提供目标状态、业务信号和当前环境门禁。
    /// @warning UI 热路径：有活动路线时只查找当前主题、分支和步骤。
    void updateGuide(UIManager* manager);

    /// @brief 显式结束当前路线，用于返回目录或关闭欢迎页。
    void stopGuide(UIManager* manager);

    /// @brief 在欢迎页正文区域绘制指定主题和可独立展开的操作分支。
    /// @param manager 提供演练服务、图片缓存和视图注册表的 UI 管理器。
    /// @param topicIndex 当前目录选中的主题索引。
    /// @warning UI 可见帧只读取目录与进度，文件访问仅来自显式操作或进度变更。
    void render(UIManager* manager, std::size_t topicIndex);

    /// @brief 启动指定配置步骤并记录本轮信号基线。
    /// @param reviewing 返回模式不接受业务信号自动推进。
    void startGuide(UIManager* manager, const Walkthrough::Topic& topic,
                    const Walkthrough::Branch& branch,
                    const Walkthrough::Step& step, bool reviewing = false);

    /// @brief 返回当前 CanonRock 写谱阶段的只读目标与错误物件反馈。
    /// @warning UI 热路径：仅返回本页已有对象，不复制谱面物件或共享所有权。
    const Walkthrough::ComposeLessonFeedback* composeLessonFeedback() const;

private:
    /// @brief 只在折线覆盖教学期间启用路径清理，离开该步骤时恢复用户设置。
    /// @param enabled 当前步骤是否需要清理折线路径上的已有物件。
    void setTemporaryPolylinePathRemoval(bool enabled);

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
        /// @brief 同步文件选择器关闭前，后续成功信号在分支入口的基线。
        std::uint64_t nextSignalRevisionAtRunStart{ 0 };
        /// @brief 关闭旧项目后重播目标时仍保留回看模式。
        bool reviewing{ false };
        /// @brief 创作教学期间固定的目标会话，避免切换标签误操作其它谱面。
        std::shared_ptr<Logic::BeatmapSession> composeSession;
        /// @brief 仅启动步骤时取得一次；UI 后续直接读已发布快照，不等待会话锁。
        std::shared_ptr<Common::Render::RenderSnapshotBuffer> composeBuffer;
        /// @brief 本轮唯一允许验收的 CanonRock 示例谱面路径键。
        std::string composeBeatmapKey;
        /// @brief 编辑阶段起始对象修订；旧谱面已有正确物件不会算新练习。
        std::uint64_t composeBaselineRevision{ 0 };
        /// @brief 最近一次已请求快照对应的对象修订。
        std::uint64_t composeCaptureRevision{ 0 };
        /// @brief 即使对象修订为零，也需在进入练习时取得一次初始物件反馈。
        bool composeCaptureRequested{ false };
        /// @brief 逻辑线程写入、UI 无阻塞轮询的一次性验收结果。
        std::shared_ptr<Logic::ComposeLessonCapture> composeCapture;
        /// @brief 首播/复播先观察到新的播放开始，避免旧帧直接完成。
        bool composePlaybackStarted{ false };
    };

    /// @brief 当前正文主题 ID，切换主题时用于重置展开项。
    std::string m_currentTopic;

    /// @brief 当前唯一展开的操作分支；空值表示全部折叠。
    std::optional<std::size_t> m_expandedBranch{ 0 };

    /// @brief 最近已提交图片准备的主题 ID。
    std::string m_preparedTopic;

    /// @brief 图片准备时采用的语言，用于检测本地化切换。
    std::string m_preparedLanguage;

    /// @brief 当前突出引导；欢迎标签隐藏时仍由 updateGuide 每帧续租。
    std::optional<ActiveGuide> m_activeGuide;
    /// @brief 低频查询结果；步骤切换后立即撤掉画布上的旧提示。
    std::unique_ptr<Walkthrough::ComposeLessonFeedback> m_composeFeedback;
    /// @brief 引导开始前的草稿区总静音状态；跨步骤保留，退出时恢复。
    std::optional<bool> m_draftAreaMutedBeforeGuide;
    /// @brief 折线覆盖教学前的用户设置；空值表示本轮没有临时覆盖。
    std::optional<bool> m_polylinePathRemovalBeforeGuide;
    /// @brief 旧项目关闭后才启动的打开项目路线，避免同一请求覆盖关闭意图。
    std::optional<ActiveGuide> m_pendingOpenGuide;
    /// @brief 启动创作教学时的可见错误；成功进入新步骤后清空。
    std::string m_guideError;
};
}  // namespace MMM::UI
