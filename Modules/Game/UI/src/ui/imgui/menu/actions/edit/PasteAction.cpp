#include "config/AppConfig.h"
#include "ui/imgui/ClipboardBridge.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuView.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 粘贴动作。
class PasteAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 从系统剪贴板同步并发布粘贴命令。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        ClipboardBridge::importEditorClipboardFromSystem();
        MenuUtil::dispatchCommand(Logic::CmdPaste{ false,
                                                   Config::AppConfig::instance()
                                                       .getEditorSettings()
                                                       .selectPastedObjects });
    }

    /// @brief 消费 Ctrl+V 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        const auto& settings =
            Config::AppConfig::instance().getEditorSettings();
        if ( ShortcutUtils::isShortcutPressed(
                 settings.shortcutConfig.mirrorPaste) ) {
            return false;
        }

        ImGuiIO& io = ImGui::GetIO();
        if ( io.KeyCtrl && !io.KeyShift &&
             ImGui::IsKeyPressed(ImGuiKey_V, false) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }
};
}  // namespace

/// @brief 创建粘贴动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createPasteAction()
{
    return std::make_unique<PasteAction>();
}

}  // namespace MMM::UI
