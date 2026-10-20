#include "ui/UIManager.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"
#include "ui/interaction/EditorInteraction.h"
#include <imgui.h>
#include <string_view>

namespace MMM::UI
{
namespace
{
/// @brief 打开新建谱面向导动作。
/// @details 通过 UIManager 查找已注册向导实例并调用 open，避免菜单层持有窗口
/// 生命周期或重复创建视图。
class OpenNewBeatmapWizardAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 跨菜单和操作方案复用的稳定动作身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view operationId() const override
    {
        return "OpenNewBeatmapWizardAction";
    }
    /// @brief 菜单提示与触发共用方案解析后的快捷键身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view shortcutActionId() const override { return "newBeatmap"; }

    /// @brief 仅在已有项目时允许创建谱面。
    /// @param context 统一菜单上下文。
    /// @return UI 已确认活动项目且不处于切换阶段时返回 true。
    /// @warning UI 热路径：只读取 UIManager 的生命周期状态。
    bool isEnabled(const MainMenuContext& context) const override
    {
        return context.sourceManager &&
               context.sourceManager->hasActiveProjectUiState() &&
               !context.sourceManager->isProjectTransitionInProgress();
    }

    /// @brief 打开新建谱面向导视图。
    /// @param context 提供可选 UIManager。
    /// @param activation 激活来源，不改变向导初态。
    /// @note 服务或视图缺失时静默忽略，避免生命周期边界崩溃。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        open(context,
             activation.m_source == MainMenuActivationSource::Shortcut
                 ? Logic::BeatmapCreateOrigin::Shortcut
                 : Logic::BeatmapCreateOrigin::FileMenu);
    }

    /// @brief 消费 Ctrl+N 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        // Ctrl+Shift+N 留给新建项目，当前动作只消费不带 Shift 的 Ctrl+N。
        if ( ShortcutUtils::isActionShortcutPressed("newBeatmap", true) ) {
            Interaction::dispatchAction(
                *this,
                context,
                MainMenuItemActivation{
                    .m_source = MainMenuActivationSource::Shortcut });
            return true;
        }
        return false;
    }

private:
    /// @brief 按指定用户入口打开已注册的新建谱面向导。
    /// @param context 提供 UIManager 与向导实例。
    /// @param origin 菜单或快捷键入口。
    /// @note 视图缺失时保持无操作，不发布虚假的向导打开阶段。
    void open(MainMenuContext& context, Logic::BeatmapCreateOrigin origin)
    {
        if ( !context.sourceManager ) return;

        auto* wizard = context.sourceManager->getView<NewBeatmapWizard>(
            "NewBeatmapWizard");
        if ( wizard ) wizard->open(origin);
    }
};
}  // namespace

/// @brief 创建打开新建谱面向导的菜单项业务处理器。
/// @return 独占所有权的无状态处理器。
/// @warning 处理器不拥有向导实例。
std::unique_ptr<IMainMenuItemActionHandler> createOpenNewBeatmapWizardAction()
{
    return std::make_unique<OpenNewBeatmapWizardAction>();
}

}  // namespace MMM::UI
