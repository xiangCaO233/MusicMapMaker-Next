#include "config/AppConfig.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 镜像选中音符动作。
class MirrorAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 获取用户配置的镜像快捷键提示。
    const char* shortcut(const MainMenuContext& context,
                         const char*            fallbackShortcut) const override
    {
        (void)context;
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        m_shortcutBuffer = ShortcutUtils::formatShortcut(shortcutConfig.mirror);
        return m_shortcutBuffer.empty() ? fallbackShortcut
                                        : m_shortcutBuffer.c_str();
    }

    /// @brief 发布镜像选中音符命令。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        MenuUtil::dispatchCommand(Logic::CmdMirrorSelected{});
    }

    /// @brief 消费用户配置的镜像快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取快捷键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        if ( !MenuUtil::canTriggerCanvasEditingShortcut() ) return false;
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        if ( ShortcutUtils::isShortcutPressed(shortcutConfig.mirror) ) {
            execute(context, MainMenuItemActivation{});
            return true;
        }
        return false;
    }

private:
    /// @brief 当前帧快捷键显示缓存。
    mutable std::string m_shortcutBuffer;
};
}  // namespace

/// @brief 创建镜像选中音符动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createMirrorAction()
{
    return std::make_unique<MirrorAction>();
}

}  // namespace MMM::UI
