#include "ui/imgui/menu/items/MainMenuViewMenu.h"
#include "config/skin/SkinConfig.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuViewActions.h"
#include "ui/imgui/menu/items/MainMenuSeparatorItem.h"
#include "ui/imgui/menu/items/MainMenuToggleItem.h"
#include <memory>
#include <utility>

namespace MMM::UI
{

/// @brief 构造视图菜单并注册默认菜单项。
MainMenuViewMenu::MainMenuViewMenu()
{
    registerItem(std::make_unique<MainMenuToggleItem>(
        "ui.view.timeline",
        MainMenuItemTextKind::TranslationKey,
        createTimelineWindowToggleAction()));
    registerItem(std::make_unique<MainMenuToggleItem>(
        "ui.view.preview",
        MainMenuItemTextKind::TranslationKey,
        createPreviewWindowToggleAction()));
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    registerItem(std::make_unique<MainMenuToggleItem>(
        "ui.view.show_tool_labels",
        MainMenuItemTextKind::TranslationKey,
        createToolLabelsToggleAction()));
    registerItem(std::make_unique<MainMenuToggleItem>(
        "ui.view.fixed_tool_window",
        MainMenuItemTextKind::TranslationKey,
        createFixedToolWindowToggleAction()));
    registerItem(std::make_unique<MainMenuToggleItem>(
        "ui.view.show_manager_labels",
        MainMenuItemTextKind::TranslationKey,
        createManagerLabelsToggleAction()));
}

/// @brief 获取视图菜单标识。
/// @return 视图菜单标识。
MainMenuId MainMenuViewMenu::id() const
{
    return MainMenuId::View;
}

/// @brief 获取视图菜单显示文本。
/// @param context 单帧主菜单上下文。
/// @return 当前语言下的视图菜单文本。
const char* MainMenuViewMenu::label(const MainMenuContext& context) const
{
    (void)context;
    return TR("ui.view").data();
}

/// @brief 遍历更新视图菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
void MainMenuViewMenu::update(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->update(context);
    }
}

/// @brief 遍历视图菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在视图菜单展开时执行。
void MainMenuViewMenu::render(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->render(context);
    }
}

/// @brief 遍历视图菜单项消费快捷键。
/// @param context 单帧主菜单上下文。
/// @return 有菜单项消费快捷键时返回 true。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
bool MainMenuViewMenu::handleShortcut(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item && item->handleShortcut(context) ) return true;
    }
    return false;
}

/// @brief 遍历渲染视图菜单项的延迟窗口。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
void MainMenuViewMenu::renderDeferred(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->renderDeferred(context);
    }
}

/// @brief 注册视图菜单项。
/// @param item 待注册菜单项。
void MainMenuViewMenu::registerItem(std::unique_ptr<IMainMenuItem> item)
{
    if ( item ) {
        m_items.push_back(std::move(item));
    }
}

}  // namespace MMM::UI
