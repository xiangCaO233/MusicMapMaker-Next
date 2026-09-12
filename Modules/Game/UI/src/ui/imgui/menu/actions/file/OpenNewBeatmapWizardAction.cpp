#include "logic/EditorEngine.h"
#include "ui/UIManager.h"
#include "ui/imgui/manager/NewBeatmapWizard.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 打开新建谱面向导动作。
/// @details 通过 UIManager 查找已注册向导实例并调用 open，避免菜单层持有窗口
/// 生命周期或重复创建视图。
class OpenNewBeatmapWizardAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在已有项目时允许创建谱面。
    /// @param context 统一菜单上下文。
    /// @return 当前项目存在时返回 true。
    /// @warning UI 热路径：只读取项目观察指针。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return Logic::EditorEngine::instance().getCurrentProject() != nullptr;
    }

    /// @brief 打开新建谱面向导视图。
    /// @param context 提供可选 UIManager。
    /// @param activation 激活来源，不改变向导初态。
    /// @note 服务或视图缺失时静默忽略，避免生命周期边界崩溃。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        if ( !context.sourceManager ) return;

        auto* wizard = context.sourceManager->getView<NewBeatmapWizard>(
            "NewBeatmapWizard");
        if ( wizard ) wizard->open();
    }

    /// @brief 消费 Ctrl+N 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        // Ctrl+Shift+N 留给新建项目，当前动作只消费不带 Shift 的 Ctrl+N。
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }
};
}  // namespace

/// @brief 创建打开新建谱面向导的菜单项业务处理器。
/// @return 独占所有权的无状态处理器。
/// @warning 处理器不拥有向导实例。
std::unique_ptr<IMainMenuItemActionHandler> createOpenNewBeatmapWizardAction()
{
    return std::make_unique<OpenNewBeatmapWizardAction>();
}

}  // namespace MMM::UI
