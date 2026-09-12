#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 选中音符节拍对齐动作。
/// @details 动作只负责统一菜单与快捷键入口，实际编辑由逻辑命令处理。
class AlignSelectedToCommonBeatsAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 发布节拍对齐逻辑命令。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源；菜单与快捷键共享同一逻辑命令。
    /// @warning 命令可能修改当前选择，必须通过命令队列保留撤销语义。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        // 只投递值语义命令，避免菜单层直接依赖画布选择实现。
        MenuUtil::dispatchCommand(Logic::CmdAlignSelectedToCommonBeats{});
    }

    /// @brief 消费 Ctrl+F 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        ImGuiIO& io = ImGui::GetIO();
        // 禁用按键重复，确保一次按下只生成一条可撤销命令。
        if ( io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }
};
}  // namespace

/// @brief 创建选中音符节拍对齐动作处理器。
/// @return 独占所有权的无状态动作处理器。
/// @note 处理器可同时服务菜单项与 Ctrl+F 快捷键。
std::unique_ptr<IMainMenuItemActionHandler>
createAlignSelectedToCommonBeatsAction()
{
    return std::make_unique<AlignSelectedToCommonBeatsAction>();
}

}  // namespace MMM::UI
