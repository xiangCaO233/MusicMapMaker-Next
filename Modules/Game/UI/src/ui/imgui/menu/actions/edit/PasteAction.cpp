#include "config/AppConfig.h"
#include "ui/imgui/ClipboardBridge.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 粘贴动作。
/// @details 先同步系统剪贴板到编辑器剪贴板，再由逻辑命令完成对象创建。
class PasteAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 从系统剪贴板同步并发布粘贴命令。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源；菜单与快捷键执行相同粘贴流程。
    /// @note 新对象是否自动选中由 EditorSettings 决定。
    /// @warning 剪贴板同步和对象创建仅应由用户显式操作触发。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        // 命令执行前刷新编辑器剪贴板，确保读取的是系统中的最新内容。
        ClipboardBridge::importEditorClipboardFromSystem();
        // 通过命令层创建对象，以保留编辑事务和撤销语义。
        MenuUtil::dispatchCommand(Logic::CmdPaste{ false,
                                                   Config::AppConfig::instance()
                                                       .getEditorSettings()
                                                       .selectPastedObjects });
    }

    /// @brief 消费 Ctrl+V 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    /// @note 镜像粘贴快捷键拥有优先级，避免同一按键组合触发两条命令。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        // 每帧读取配置引用，不复制快捷键集合。
        const auto& settings =
            Config::AppConfig::instance().getEditorSettings();
        if ( ShortcutUtils::isShortcutPressed(
                 settings.shortcutConfig.mirrorPaste) ) {
            // 已命中更具体的镜像粘贴组合时，普通粘贴主动让出处理权。
            return false;
        }

        ImGuiIO& io = ImGui::GetIO();
        // 排除 Shift，防止与常见的扩展粘贴组合发生重叠。
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
/// @return 独占所有权的无状态处理器。
/// @note 系统剪贴板只在动作执行时访问。
/// @warning 处理器应在 ImGui 所属 UI 线程调用。
std::unique_ptr<IMainMenuItemActionHandler> createPasteAction()
{
    return std::make_unique<PasteAction>();
}

}  // namespace MMM::UI
