#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "ui/imgui/ClipboardBridge.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include "ui/interaction/EditorInteraction.h"
#include <imgui.h>
#include <string_view>

namespace MMM::UI
{
namespace
{
/// @brief 粘贴动作。
/// @details 先同步系统剪贴板到编辑器剪贴板，再由逻辑命令完成对象创建。
class PasteAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 跨菜单和操作方案复用的稳定动作身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view operationId() const override { return "PasteAction"; }
    /// @brief 菜单提示与触发共用方案解析后的快捷键身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view shortcutActionId() const override { return "paste"; }

    /// @brief 从系统剪贴板同步并发布粘贴命令。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源；菜单与快捷键执行相同粘贴流程。
    /// @note 新对象是否自动选中由 EditorSettings 决定。
    /// @warning 剪贴板同步和对象创建仅应由用户显式操作触发。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        // 命令执行前刷新编辑器剪贴板，确保读取的是系统中的最新内容。
        ClipboardBridge::importEditorClipboardFromSystem();
        // 通过命令层创建对象，以保留编辑事务和撤销语义。
        MenuUtil::dispatchCommand(Logic::CmdPaste{ false,
                                                   Config::AppConfig::instance()
                                                       .getEditorSettings()
                                                       .selectPastedObjects,
                                                   activation.m_pasteTime });
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
        if ( ShortcutUtils::isActionShortcutPressed("mirrorPaste") ) {
            // 已命中更具体的镜像粘贴组合时，普通粘贴主动让出处理权。
            return false;
        }

        // 排除 Shift，防止与常见的扩展粘贴组合发生重叠。
        if ( ShortcutUtils::isActionShortcutPressed("paste", false) ) {
            Interaction::dispatchAction(
                *this,
                context,
                MainMenuItemActivation{
                    .m_source = MainMenuActivationSource::Shortcut });
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
