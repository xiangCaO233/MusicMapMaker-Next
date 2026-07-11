#include "ui/imgui/menu/items/MainMenuActionItem.h"
#include "config/skin/SkinConfig.h"
#include "ui/imgui/menu/items/MainMenuItemUtils.h"
#include <utility>

namespace MMM::UI
{

/// @brief 构造普通主菜单项。
/// @param icon 菜单项默认图标文本，可为空。
/// @param label 菜单项文本或翻译键。
/// @param textKind 菜单项文本来源。
/// @param shortcut 菜单项默认快捷键提示，可为空。
/// @param actionHandler 菜单项业务处理器。
MainMenuActionItem::MainMenuActionItem(
    const char* icon, std::string label, MainMenuItemTextKind textKind,
    const char*                                 shortcut,
    std::unique_ptr<IMainMenuItemActionHandler> actionHandler)
    : m_icon(icon)
    , m_label(std::move(label))
    , m_textKind(textKind)
    , m_shortcut(shortcut)
    , m_actionHandler(std::move(actionHandler))
{
}

/// @brief 更新菜单项业务处理器跨帧状态。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给 action handler。
void MainMenuActionItem::update(MainMenuContext& context)
{
    if ( m_actionHandler ) {
        m_actionHandler->update(context);
    }
}

/// @brief 绘制菜单项并在点击时执行自身 action handler。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在所属菜单展开时执行。
void MainMenuActionItem::render(MainMenuContext& context)
{
    const bool enabled =
        !m_actionHandler || m_actionHandler->isEnabled(context);
    const char* icon =
        m_actionHandler ? m_actionHandler->icon(context, m_icon) : m_icon;
    const char* shortcut = m_actionHandler
                               ? m_actionHandler->shortcut(context, m_shortcut)
                               : m_shortcut;
    if ( renderMainMenuIconItem(icon, resolveLabel(), shortcut, enabled) &&
         m_actionHandler ) {
        m_actionHandler->execute(context, MainMenuItemActivation{});
    }
}

/// @brief 转发快捷键消费给业务处理器。
/// @param context 单帧主菜单上下文。
/// @return 业务处理器消费快捷键时返回 true。
/// @warning UI 热路径：每帧执行；仅转发给 action handler。
bool MainMenuActionItem::handleShortcut(MainMenuContext& context)
{
    if ( !m_actionHandler || !m_actionHandler->isEnabled(context) ) {
        return false;
    }
    return m_actionHandler->handleShortcut(context);
}

/// @brief 渲染菜单项业务处理器延迟窗口。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给 action handler。
void MainMenuActionItem::renderDeferred(MainMenuContext& context)
{
    if ( m_actionHandler ) {
        m_actionHandler->renderDeferred(context);
    }
}

/// @brief 获取当前帧的显示文本。
/// @return 当前帧应显示的菜单项文本。
const char* MainMenuActionItem::resolveLabel() const
{
    if ( m_textKind == MainMenuItemTextKind::Literal ) {
        return m_label.c_str();
    }
    return TR(m_label.c_str()).data();
}

}  // namespace MMM::UI
