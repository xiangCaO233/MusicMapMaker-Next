#include "ui/UIManager.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "ui/imgui/menu/actions/tools/BpmShortcutFocus.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/interaction/EditorInteraction.h"
#include <imgui.h>
#include <string_view>

namespace MMM::UI
{
namespace
{
/// @brief 重做动作。
/// @details 同时兼容 Ctrl+Y 与 Ctrl+Shift+Z，两种手势统一发布 CmdRedo，避免逻辑
/// 层区分平台输入习惯。
/// BPM 工具聚焦时，两种手势均切换到测量历史；历史为空仍消费按键，
/// 保证背后的活动谱面不会在工具操作期间被意外重做。
class RedoAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 跨菜单和操作方案复用的稳定动作身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view operationId() const override { return "RedoAction"; }
    /// @brief 菜单提示与触发共用方案解析后的快捷键身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view shortcutActionId() const override { return "redo"; }

    /// @brief 发布重做命令。
    /// @param context 统一菜单上下文。
    /// @param activation 激活来源，不改变重做语义。
    /// @note 历史为空时由逻辑层忽略，不在 UI 维护额外可用状态。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        // 焦点路由在动作执行阶段统一处理，方案拦截前不触碰测量工具历史。
        if ( activation.m_target ==
                 MainMenuActivationTarget::BpmMeasurementTool ||
             (activation.m_source == MainMenuActivationSource::Shortcut &&
              isBpmMeasurementToolFocused(ImGui::GetCurrentContext())) ) {
            // 重做历史为空时仍由工具消费，避免误改背后的谱面。
            // 焦点先于工具实例判定，关闭窗口后不会沿用旧路由。
            auto* tool =
                context.sourceManager
                    ? context.sourceManager->getView<BpmMeasurementToolView>(
                          "BpmMeasurementTool")
                    : nullptr;
            if ( tool && tool->canRedoMeasurement() ) {
                // 历史可用性只在工具内维护，不推测谱面重做栈状态。
                tool->redoMeasurementFromShortcut();
            }
            return;
        }
        MenuUtil::dispatchCommand(Logic::CmdRedo{});
    }

    /// @brief 消费 Ctrl+Y 或 Ctrl+Shift+Z 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    /// @note Ctrl+Shift+Z 与撤销动作的 Shift 排除条件保持互斥。
    /// @note 文本输入与弹窗在菜单入口阻断；此处不读取其他视图状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        const bool redoByY = ShortcutUtils::isActionShortcutPressed("redo");
        const bool redoByShiftZ =
            ShortcutUtils::isActionShortcutPressed("redoAlternative");
        // 两个组合键共享一次执行与消费路径，单帧最多发布一个命令。
        // 无匹配时保留输入给后续动作，不改变编辑器历史。
        // Alt 与 Super 不属于既有重做手势，避免抢占其他组合键。
        if ( redoByY || redoByShiftZ ) {

            if ( !isBpmMeasurementToolFocused(ImGui::GetCurrentContext()) &&
                 !MenuUtil::canTriggerCanvasEditingShortcut() )
                return false;
            Interaction::dispatchAction(
                *this,
                context,
                MainMenuItemActivation{
                    .m_source = MainMenuActivationSource::Shortcut });
            return true;
        }
        return false;
    }
};
}  // namespace

/// @brief 创建重做动作处理器。
/// @return 独占所有权的无状态处理器。
std::unique_ptr<IMainMenuItemActionHandler> createRedoAction()
{
    return std::make_unique<RedoAction>();
}

}  // namespace MMM::UI
