#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 重做动作。
/// @details 同时兼容 Ctrl+Y 与 Ctrl+Shift+Z，两种手势统一发布 CmdRedo，避免逻辑
/// 层区分平台输入习惯。
class RedoAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 发布重做命令。
    /// @param context 统一菜单上下文。
    /// @param activation 激活来源，不改变重做语义。
    /// @note 历史为空时由逻辑层忽略，不在 UI 维护额外可用状态。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        MenuUtil::dispatchCommand(Logic::CmdRedo{});
    }

    /// @brief 消费 Ctrl+Y 或 Ctrl+Shift+Z 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    /// @note Ctrl+Shift+Z 与撤销动作的 Shift 排除条件保持互斥。
    bool handleShortcut(MainMenuContext& context) override
    {
        // 文本编辑或弹窗占用输入时不消费常见编辑快捷键。
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        ImGuiIO&   io      = ImGui::GetIO();
        const bool redoByY = io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y);
        const bool redoByShiftZ =
            io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z);
        // 两个组合键共享一次执行与消费路径，单帧最多发布一个命令。
        if ( redoByY || redoByShiftZ ) {
            execute(context, MainMenuItemActivation{});
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
