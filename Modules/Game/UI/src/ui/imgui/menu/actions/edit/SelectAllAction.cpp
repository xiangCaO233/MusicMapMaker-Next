#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 全选鼠标所在轨道区动作。
class SelectAllAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 发布当前轨道区全选命令。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        MenuUtil::dispatchCommand(Logic::CmdSelectAll{
            .scope = Logic::SelectAllScope::CurrentTrackArea,
        });
    }

    /// @brief 消费 Ctrl+A 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && !io.KeyShift &&
             ImGui::IsKeyPressed(ImGuiKey_A, false) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }
};

/// @brief 全选所有轨道区物件动作。
class SelectAllObjectsAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 发布所有轨道区全选命令。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        MenuUtil::dispatchCommand(Logic::CmdSelectAll{
            .scope = Logic::SelectAllScope::AllTrackAreas,
        });
    }

    /// @brief 消费 Ctrl+Shift+A 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && io.KeyShift &&
             ImGui::IsKeyPressed(ImGuiKey_A, false) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }
};
}  // namespace

/// @brief 创建当前轨道区全选动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createSelectAllAction()
{
    return std::make_unique<SelectAllAction>();
}

/// @brief 创建所有轨道区全选动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createSelectAllObjectsAction()
{
    return std::make_unique<SelectAllObjectsAction>();
}

}  // namespace MMM::UI
