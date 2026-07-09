#include "ui/imgui/menu/MainMenuInterfaces.h"
#include "ui/imgui/menu/items/MainMenuEdit.h"
#include "ui/imgui/menu/items/MainMenuFile.h"
#include "ui/imgui/menu/items/MainMenuHelp.h"
#include "ui/imgui/menu/items/MainMenuTools.h"
#include "ui/imgui/menu/items/MainMenuViewMenu.h"
#include <memory>
#include <vector>

namespace MMM::UI
{

/// @brief 创建默认一级主菜单注册表。
/// @return 默认一级主菜单列表。
std::vector<std::unique_ptr<IMainMenu>> createDefaultMainMenus()
{
    std::vector<std::unique_ptr<IMainMenu>> menus;
    menus.reserve(MAIN_MENU_ID_COUNT);
    menus.push_back(std::make_unique<MainMenuFile>());
    menus.push_back(std::make_unique<MainMenuEdit>());
    menus.push_back(std::make_unique<MainMenuTools>());
    menus.push_back(std::make_unique<MainMenuViewMenu>());
    menus.push_back(std::make_unique<MainMenuHelp>());
    return menus;
}

}  // namespace MMM::UI
