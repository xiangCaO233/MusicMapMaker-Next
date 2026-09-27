#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 全选鼠标所在轨道区动作。
/// @details 当前轨道区由逻辑层根据画布交互状态解析，菜单层不缓存轨道标识。
class SelectAllAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 发布当前轨道区全选命令。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源，不改变选择范围。
    /// @warning 必须经命令层修改选择，保持 UI 与会话状态一致。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        // 显式指定 CurrentTrackArea，避免默认值变化扩大选择范围。
        MenuUtil::dispatchCommand(Logic::CmdSelectAll{
            .scope = Logic::SelectAllScope::CurrentTrackArea,
        });
    }

    /// @brief 消费 Ctrl+A 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    /// @note Shift 组合留给跨轨道全选动作处理。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        ImGuiIO& io = ImGui::GetIO();
        // 排除 Shift，保证 Ctrl+A 与 Ctrl+Shift+A 的职责互斥。
        if ( io.KeyCtrl && !io.KeyShift &&
             ImGui::IsKeyPressed(ImGuiKey_A, false) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }
};

/// @brief 全选所有轨道区物件动作。
/// @details 跨轨道范围由 CmdSelectAll 明确表达，不依赖鼠标所在区域。
class SelectAllObjectsAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 发布所有轨道区全选命令。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源，不改变选择范围。
    /// @warning 选择更新必须留在逻辑命令处理链中。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        // 显式使用 AllTrackAreas，确保所有可编辑轨道均纳入选择。
        MenuUtil::dispatchCommand(Logic::CmdSelectAll{
            .scope = Logic::SelectAllScope::AllTrackAreas,
        });
    }

    /// @brief 消费 Ctrl+Shift+A 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    /// @note 只有 Ctrl 与 Shift 同时按下时才消费输入。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        ImGuiIO& io = ImGui::GetIO();
        // 组合判断与当前轨道区动作互斥，避免同帧发出两条选择命令。
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
/// @return 独占所有权的无状态处理器。
std::unique_ptr<IMainMenuItemActionHandler> createSelectAllAction()
{
    return std::make_unique<SelectAllAction>();
}

/// @brief 创建所有轨道区全选动作处理器。
/// @return 独占所有权的无状态处理器。
std::unique_ptr<IMainMenuItemActionHandler> createSelectAllObjectsAction()
{
    return std::make_unique<SelectAllObjectsAction>();
}

}  // namespace MMM::UI
