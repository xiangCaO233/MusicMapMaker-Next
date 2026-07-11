#include "ui/imgui/menu/interfaces/IMainMenuItemActionHandler.h"

namespace MMM::UI
{

/// @brief 默认不更新菜单项业务处理器跨帧状态。
/// @param context 单帧主菜单上下文。
void IMainMenuItemActionHandler::update(MainMenuContext& context)
{
    (void)context;
}

/// @brief 默认不消费菜单项业务处理器快捷键。
/// @param context 单帧主菜单上下文。
/// @return 始终返回 false。
bool IMainMenuItemActionHandler::handleShortcut(MainMenuContext& context)
{
    (void)context;
    return false;
}

/// @brief 获取当前菜单项是否可用。
/// @param context 单帧主菜单上下文。
/// @return 可点击时返回 true。
bool IMainMenuItemActionHandler::isEnabled(const MainMenuContext& context) const
{
    (void)context;
    return true;
}

/// @brief 获取当前菜单项图标。
/// @param context 单帧主菜单上下文。
/// @param fallbackIcon 菜单项配置的默认图标。
/// @return 当前帧应显示的图标文本。
const char* IMainMenuItemActionHandler::icon(const MainMenuContext& context,
                                             const char* fallbackIcon) const
{
    (void)context;
    return fallbackIcon;
}

/// @brief 获取当前菜单项快捷键提示。
/// @param context 单帧主菜单上下文。
/// @param fallbackShortcut 菜单项配置的默认快捷键提示。
/// @return 当前帧应显示的快捷键提示，可为空。
const char* IMainMenuItemActionHandler::shortcut(
    const MainMenuContext& context, const char* fallbackShortcut) const
{
    (void)context;
    return fallbackShortcut;
}

/// @brief 默认不渲染菜单项业务处理器延迟窗口。
/// @param context 单帧主菜单上下文。
void IMainMenuItemActionHandler::renderDeferred(MainMenuContext& context)
{
    (void)context;
}

}  // namespace MMM::UI
