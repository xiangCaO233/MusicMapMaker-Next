#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"

#include "ui/UIManager.h"
#include "ui/imgui/manager/NewProjectWizard.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 打开新建项目向导动作。
class OpenNewProjectWizardAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 标记下一次延迟渲染打开新建项目向导。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        m_pendingOpen = true;
    }

    /// @brief 消费 Ctrl+Shift+N 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        (void)context;
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N) ) {
            m_pendingOpen = true;
            return true;
        }
        return false;
    }

    /// @brief 在菜单栏窗口外打开新建项目向导。
    /// @warning UI 热路径：每帧只检查布尔标志，实际打开仅由用户点击触发。
    void renderDeferred(MainMenuContext& context) override
    {
        if ( !m_pendingOpen ) return;
        if ( !context.sourceManager ) return;

        auto* wizard = context.sourceManager->getView<NewProjectWizard>(
            "NewProjectWizard");
        if ( wizard ) {
            wizard->open();
            m_pendingOpen = false;
        }
    }

private:
    /// @brief 是否延迟打开新建项目向导。
    bool m_pendingOpen = false;
};
}  // namespace

/// @brief 创建打开新建项目向导的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenNewProjectWizardAction()
{
    return std::make_unique<OpenNewProjectWizardAction>();
}

}  // namespace MMM::UI
