#pragma once

#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/interfaces/IMainMenu.h"
#include "ui/imgui/menu/interfaces/IMainMenuItem.h"
#include "ui/imgui/menu/interfaces/IMainMenuItemActionHandler.h"
#include "ui/imgui/menu/interfaces/IMainMenuToggleItemActionHandler.h"

#include <memory>
#include <vector>

namespace MMM::UI
{

/// @brief 创建默认一级主菜单注册表。
/// @return 默认一级主菜单列表。
///
/// @details 返回顺序决定菜单栏从左到右的稳定布局；每个菜单以独占所有权交给
/// 主菜单宿主，调用方可以统一执行 update、快捷键和渲染阶段。
/// @note 返回容器不共享菜单实例，生命周期完全由调用方管理。
std::vector<std::unique_ptr<IMainMenu>> createDefaultMainMenus();

}  // namespace MMM::UI
