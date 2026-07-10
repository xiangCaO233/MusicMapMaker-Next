#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 选中音符节拍对齐动作。
class AlignSelectedToCommonBeatsAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 发布节拍对齐逻辑命令。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
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
        if ( io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }
};
}  // namespace

/// @brief 创建选中音符节拍对齐动作处理器。
std::unique_ptr<IMainMenuItemActionHandler>
createAlignSelectedToCommonBeatsAction()
{
    return std::make_unique<AlignSelectedToCommonBeatsAction>();
}

}  // namespace MMM::UI
