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
///
/// @details 菜单按文件、编辑、工具、查看、帮助的产品顺序构造。注册表只负责
/// 默认组成与所有权，不在此执行 ImGui 绘制或读取运行期上下文。
///
/// @note 新增一级菜单时必须同步 MAIN_MENU_ID_COUNT，保持 reserve
/// 与注册数量一致。
std::vector<std::unique_ptr<IMainMenu>> createDefaultMainMenus()
{
    // 预留已知一级菜单数量，构造过程中无需扩容 unique_ptr 容器。
    std::vector<std::unique_ptr<IMainMenu>> menus;
    menus.reserve(MAIN_MENU_ID_COUNT);
    // 插入顺序同时是菜单栏显示顺序，调整时需同步交互与测试预期。
    menus.push_back(std::make_unique<MainMenuFile>());
    menus.push_back(std::make_unique<MainMenuEdit>());
    menus.push_back(std::make_unique<MainMenuTools>());
    menus.push_back(std::make_unique<MainMenuViewMenu>());
    // 帮助菜单固定置于末尾，符合桌面应用常见导航习惯。
    menus.push_back(std::make_unique<MainMenuHelp>());
    return menus;
}

}  // namespace MMM::UI
