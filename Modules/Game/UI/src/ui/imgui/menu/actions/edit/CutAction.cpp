#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 剪切动作。
/// @details 只向逻辑命令队列发布 CmdCut，实际删除与剪贴板更新保持在可撤销的
/// 编辑事务中完成。
class CutAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 发布剪切命令。
    /// @param context 统一菜单上下文。
    /// @param activation 激活来源，不改变剪切命令。
    /// @note 是否存在可剪切选择由逻辑命令处理器判断。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        MenuUtil::dispatchCommand(Logic::CmdCut{});
    }

    /// @brief 消费 Ctrl+X 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    /// @note repeat=false 防止长按连续删除选择内容。
    bool handleShortcut(MainMenuContext& context) override
    {
        // 仅在画布编辑快捷键可用时消费 Ctrl+X，保护文本框剪切行为。
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }
};
}  // namespace

/// @brief 创建剪切动作处理器。
/// @return 独占所有权的无状态处理器。
std::unique_ptr<IMainMenuItemActionHandler> createCutAction()
{
    return std::make_unique<CutAction>();
}

}  // namespace MMM::UI
