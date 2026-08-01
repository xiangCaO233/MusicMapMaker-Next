#include "logic/EditorEngine.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 打开音频导入选择器动作。
class OpenAudioImportAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在已有项目时允许导入音频。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return Logic::EditorEngine::instance().getCurrentProject() != nullptr;
    }

    /// @brief 打开音频导入选择器。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        MenuUtil::openAudioImportPicker();
    }

    /// @brief 消费 Ctrl+I 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && !io.KeyShift &&
             ImGui::IsKeyPressed(ImGuiKey_I, false) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }
};
}  // namespace

/// @brief 创建打开音频导入选择器的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenAudioImportAction()
{
    return std::make_unique<OpenAudioImportAction>();
}

}  // namespace MMM::UI
