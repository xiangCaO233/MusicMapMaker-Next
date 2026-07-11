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
std::vector<std::unique_ptr<IMainMenu>> createDefaultMainMenus();

}  // namespace MMM::UI
