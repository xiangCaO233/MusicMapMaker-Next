#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 撤销动作。
/// @details 菜单和快捷键均发布 CmdUndo，历史栈可用性与实际状态转换由逻辑层
/// 决定，UI 不复制撤销状态。
class UndoAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 发布撤销命令。
    /// @param context 统一菜单上下文。
    /// @param activation 激活来源，不改变撤销目标。
    /// @note 历史为空时由逻辑层安全忽略命令。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        MenuUtil::dispatchCommand(Logic::CmdUndo{});
    }

    /// @brief 消费 Ctrl+Z 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    /// @note 默认按键查询不启用重复触发，避免长按跨越多个历史项。
    bool handleShortcut(MainMenuContext& context) override
    {
        // 排除 Shift 以把 Ctrl+Shift+Z 明确留给 RedoAction。
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
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
