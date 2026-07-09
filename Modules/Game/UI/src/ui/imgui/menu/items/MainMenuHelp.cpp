#include "ui/imgui/menu/items/MainMenuHelp.h"
#include "config/skin/SkinConfig.h"
#include "ui/Icons.h"
#include "ui/imgui/menu/actions/MainMenuHelpActions.h"
#include "ui/imgui/menu/items/MainMenuActionItem.h"
#include <memory>
#include <utility>

namespace MMM::UI
{

/// @brief 构造帮助菜单并注册默认菜单项。
MainMenuHelp::MainMenuHelp()
{
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_DOWNLOAD,
        "ui.help.check_update",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createCheckUpdateAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_INFO_CIRCLE,
        "ui.help.about",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createShowAboutAction()));
}

/// @brief 获取帮助菜单标识。
/// @return 帮助菜单标识。
MainMenuId MainMenuHelp::id() const
{
    return MainMenuId::Help;
}

/// @brief 获取帮助菜单显示文本。
/// @param context 单帧主菜单上下文。
/// @return 当前语言下的帮助菜单文本。
const char* MainMenuHelp::label(const MainMenuContext& context) const
{
    (void)context;
    return TR("ui.help");
}

/// @brief 遍历更新帮助菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
void MainMenuHelp::update(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->update(context);
    }
}

/// @brief 遍历帮助菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在帮助菜单展开时执行。
void MainMenuHelp::render(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->render(context);
    }
}

/// @brief 遍历帮助菜单项消费快捷键。
/// @param context 单帧主菜单上下文。
/// @return 有菜单项消费快捷键时返回 true。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
bool MainMenuHelp::handleShortcut(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item && item->handleShortcut(context) ) return true;
    }
    return false;
}

/// @brief 遍历渲染帮助菜单项的延迟窗口。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
void MainMenuHelp::renderDeferred(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->renderDeferred(context);
    }
}

/// @brief 注册帮助菜单项。
/// @param item 待注册菜单项。
void MainMenuHelp::registerItem(std::unique_ptr<IMainMenuItem> item)
{
    if ( item ) {
        m_items.push_back(std::move(item));
    }
}

}  // namespace MMM::UI
