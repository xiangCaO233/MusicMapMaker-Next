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
class OpenNewBeatmapWizardAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在已有项目时允许创建谱面。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return Logic::EditorEngine::instance().getCurrentProject() != nullptr;
    }

    /// @brief 打开新建谱面向导视图。
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
std::unique_ptr<IMainMenuItemActionHandler> createOpenNewBeatmapWizardAction()
{
    return std::make_unique<OpenNewBeatmapWizardAction>();
}

}  // namespace MMM::UI
