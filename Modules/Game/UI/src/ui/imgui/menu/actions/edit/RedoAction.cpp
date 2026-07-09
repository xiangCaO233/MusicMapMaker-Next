#include "ui/imgui/menu/MainMenuView.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 重做动作。
class RedoAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 发布重做命令。
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
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        ImGuiIO&   io      = ImGui::GetIO();
        const bool redoByY = io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y);
        const bool redoByShiftZ =
            io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z);
        if ( redoByY || redoByShiftZ ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }
};
}  // namespace

/// @brief 创建重做动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createRedoAction()
{
    return std::make_unique<RedoAction>();
}

}  // namespace MMM::UI
