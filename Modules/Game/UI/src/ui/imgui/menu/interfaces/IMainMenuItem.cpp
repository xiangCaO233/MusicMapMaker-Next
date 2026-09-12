#include "ui/imgui/menu/interfaces/IMainMenuItem.h"

namespace MMM::UI
{

/// @brief 默认不更新菜单项跨帧状态。
/// @param context 单帧主菜单上下文。
/// @note 无状态菜单项无需覆盖该阶段，派生类仍可按需实现。
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
