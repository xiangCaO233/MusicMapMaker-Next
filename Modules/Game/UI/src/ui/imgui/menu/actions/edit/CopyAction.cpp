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
/// @brief 复制动作。
/// @details 菜单点击与快捷键统一发布 CmdCopy，由逻辑层读取当前画布选择并写入
/// 剪贴板；UI 动作不直接访问谱面对象。
class CopyAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 跨菜单和操作方案复用的稳定动作身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view operationId() const override { return "CopyAction"; }
    /// @brief 菜单提示与触发共用方案解析后的快捷键身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view shortcutActionId() const override { return "copy"; }

    /// @brief 发布复制命令。
    /// @param context 统一菜单上下文，命令总线由 MenuUtil 管理。
    /// @param activation 激活来源，不改变复制语义。
    /// @note 命令异步交给逻辑层，本函数不假设当前一定存在选择。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        MenuUtil::dispatchCommand(Logic::CmdCopy{});
    }

    /// @brief 消费 Ctrl+C 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    /// @note repeat=false 保证一次按下只发布一个复制命令。
    bool handleShortcut(MainMenuContext& context) override
    {
        // 文本输入或非画布焦点时不得抢占系统 Ctrl+C。
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        if ( ShortcutUtils::isActionShortcutPressed("copy", false) ) {
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

/// @brief 创建复制动作处理器。
/// @return 独占所有权的无状态处理器。
std::unique_ptr<IMainMenuItemActionHandler> createCopyAction()
{
    return std::make_unique<CopyAction>();
}

}  // namespace MMM::UI
