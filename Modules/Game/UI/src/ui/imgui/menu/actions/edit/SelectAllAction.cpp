#include "ui/imgui/menu/MainMenuView.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 全选动作。
class SelectAllAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 发布全选命令。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        MenuUtil::dispatchCommand(Logic::CmdSelectAll{});
    }

    /// @brief 消费 Ctrl+A 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }
};
}  // namespace

/// @brief 创建全选动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createSelectAllAction()
{
    return std::make_unique<SelectAllAction>();
}

}  // namespace MMM::UI
