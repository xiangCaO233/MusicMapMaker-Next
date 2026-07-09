#include "ui/imgui/menu/interfaces/IMainMenuItem.h"

namespace MMM::UI
{

/// @brief 默认不更新菜单项跨帧状态。
/// @param context 单帧主菜单上下文。
void IMainMenuItem::update(MainMenuContext& context)
{
    (void)context;
}

/// @brief 默认不消费菜单项快捷键。
/// @param context 单帧主菜单上下文。
/// @return 始终返回 false。
bool IMainMenuItem::handleShortcut(MainMenuContext& context)
{
    (void)context;
    return false;
}

/// @brief 默认不渲染菜单项延迟窗口。
/// @param context 单帧主菜单上下文。
void IMainMenuItem::renderDeferred(MainMenuContext& context)
{
    (void)context;
}

}  // namespace MMM::UI
