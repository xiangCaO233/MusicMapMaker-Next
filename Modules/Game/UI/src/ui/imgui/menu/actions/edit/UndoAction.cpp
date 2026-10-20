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
/// @brief 撤销动作。
/// @details 菜单和快捷键均发布 CmdUndo，历史栈可用性与实际状态转换由逻辑层
/// 决定，UI 不复制撤销状态。
/// BPM 工具聚焦时快捷键属于工具测量历史；该历史为空时也不能
/// 转交活动谱面。菜单点击仍保留原有编辑器命令语义。
class UndoAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 跨菜单和操作方案复用的稳定动作身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    /// @details 谱面与测量工具共用身份，通过激活目标区分各自历史。
    std::string_view operationId() const override { return "UndoAction"; }
    /// @brief 菜单提示与触发共用方案解析后的快捷键身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view shortcutActionId() const override { return "undo"; }

    /// @brief 发布撤销命令。
    /// @param context 统一菜单上下文。
    /// @param activation 激活来源，不改变撤销目标。
    /// @note 历史为空时由逻辑层安全忽略命令。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        // 焦点路由在动作执行阶段统一处理，方案拦截前不触碰测量工具历史。
        if ( activation.m_target ==
                 MainMenuActivationTarget::BpmMeasurementTool ||
             (activation.m_source == MainMenuActivationSource::Shortcut &&
              isBpmMeasurementToolFocused(ImGui::GetCurrentContext())) ) {
            // 工具窗口拥有历史快捷键；无历史项时仍消费本次按键。
            // 视图指针只借用当前 UI 帧，不延长工具窗口生命周期。
            auto* tool =
                context.sourceManager
                    ? context.sourceManager->getView<BpmMeasurementToolView>(
                          "BpmMeasurementTool")
                    : nullptr;
            if ( tool && tool->canUndoMeasurement() ) {
                // 测量历史由工具独立维护，编辑器撤销栈不参与决策。
                tool->undoMeasurementFromShortcut();
            }
            return;
        }
        MenuUtil::dispatchCommand(Logic::CmdUndo{});
    }

    /// @brief 消费 Ctrl+Z 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    /// @note 默认按键查询不启用重复触发，避免长按跨越多个历史项。
    /// @note 文本输入与弹窗由主菜单入口拦截，此处决定工具焦点所有权。
    bool handleShortcut(MainMenuContext& context) override
    {
        // 先判定按键和工具焦点；工具历史为空也不能穿透到活动谱面。
        if ( !ShortcutUtils::isActionShortcutPressed("undo") ) {
            return false;
        }

        if ( !isBpmMeasurementToolFocused(ImGui::GetCurrentContext()) &&
             !MenuUtil::canTriggerCanvasEditingShortcut() )
            return false;
        Interaction::dispatchAction(
            *this,
            context,
            MainMenuItemActivation{ .m_source =
                                        MainMenuActivationSource::Shortcut });
        return true;
    }
};
}  // namespace

/// @brief 创建撤销动作处理器。
/// @return 独占所有权的无状态处理器。
std::unique_ptr<IMainMenuItemActionHandler> createUndoAction()
{
    return std::make_unique<UndoAction>();
}

}  // namespace MMM::UI
