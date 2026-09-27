#include "event/ui/UISettingsTabEvent.h"
#include "ui/UIManager.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"

namespace MMM::UI
{
namespace
{
/// @brief 打开谱面设置动作。
/// @details 动作只负责选择 SettingsTab::Beatmap 并请求 UIManager 打开窗口，不在
/// 菜单层读取或修改谱面配置。
class OpenBeatmapSettingsAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 打开设置窗口的谱面页。
    /// @param context 提供可选 UIManager 的单帧上下文。
    /// @param activation 激活来源，不改变目标设置页。
    /// @note sourceManager 不可用时静默忽略，避免启动或关闭阶段访问失效 UI。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        // 由统一窗口入口处理已有窗口聚焦与标签切换。
        if ( context.sourceManager ) {
            context.sourceManager->openSettingsWindow(
                Event::SettingsTab::Beatmap);
        }
    }
};
}  // namespace

/// @brief 创建打开谱面设置动作处理器。
/// @return 独占所有权的无状态处理器。
/// @note 处理器生命周期由主菜单注册表管理。
/// @warning 返回对象不持有 UIManager，避免延长界面服务生命周期。
std::unique_ptr<IMainMenuItemActionHandler> createOpenBeatmapSettingsAction()
{
    return std::make_unique<OpenBeatmapSettingsAction>();
}

}  // namespace MMM::UI
