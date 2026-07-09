#include "ui/imgui/menu/interfaces/IMainMenuToggleItemActionHandler.h"

namespace MMM::UI
{

/// @brief 默认不更新勾选菜单项业务处理器跨帧状态。
/// @param context 单帧主菜单上下文。
void IMainMenuToggleItemActionHandler::update(MainMenuContext& context)
{
    (void)context;
}

/// @brief 默认不消费勾选菜单项业务处理器快捷键。
/// @param context 单帧主菜单上下文。
/// @return 始终返回 false。
bool IMainMenuToggleItemActionHandler::handleShortcut(MainMenuContext& context)
{
    (void)context;
    return false;
}

/// @brief 获取当前勾选项是否可用。
/// @param context 单帧主菜单上下文。
/// @return 可交互时返回 true。
bool IMainMenuToggleItemActionHandler::isEnabled(
    const MainMenuContext& context) const
{
    (void)context;
    return true;
}

/// @brief 默认不渲染勾选菜单项业务处理器延迟窗口。
/// @param context 单帧主菜单上下文。
void IMainMenuToggleItemActionHandler::renderDeferred(MainMenuContext& context)
{
    (void)context;
}

}  // namespace MMM::UI
