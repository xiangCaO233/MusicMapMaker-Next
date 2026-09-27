#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 复制动作。
/// @details 菜单点击与快捷键统一发布 CmdCopy，由逻辑层读取当前画布选择并写入
/// 剪贴板；UI 动作不直接访问谱面对象。
class CopyAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 发布复制命令。
    /// @param context 统一菜单上下文，命令总线由 MenuUtil 管理。
    /// @param activation 激活来源，不改变复制语义。
    /// @note 命令异步交给逻辑层，本函数不假设当前一定存在选择。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        MenuUtil::dispatchCommand(Logic::CmdCopy{});
    }

    /// @brief 消费 Ctrl+C 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    /// @note repeat=false 保证一次按下只发布一个复制命令。
    bool handleShortcut(MainMenuContext& context) override
    {
        // 文本输入或非画布焦点时不得抢占系统 Ctrl+C。
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }
};
}  // namespace

/// @brief 创建复制动作处理器。
/// @return 独占所有权的无状态处理器。
std::unique_ptr<IMainMenuItemActionHandler> createCopyAction()
{
    return std::make_unique<CopyAction>();
}

}  // namespace MMM::UI
