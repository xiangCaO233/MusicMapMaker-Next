#include "ui/imgui/menu/items/MainMenuToggleItem.h"
#include "config/skin/SkinConfig.h"
#include "ui/imgui/menu/items/MainMenuItemUtils.h"
#include "ui/utils/UIWidgetUtils.h"
#include <utility>

namespace MMM::UI
{

/// @brief 构造勾选主菜单项。
/// @param label 菜单项文本或翻译键。
/// @param textKind 菜单项文本来源。
/// @param actionHandler 勾选菜单项业务处理器。
/// @param icon 菜单项图标文本，可为空；须在菜单项生命周期内有效。
MainMenuToggleItem::MainMenuToggleItem(
    std::string label, MainMenuItemTextKind textKind,
    std::unique_ptr<IMainMenuToggleItemActionHandler> actionHandler,
    const char*                                       icon)
    : m_label(std::move(label))
    , m_textKind(textKind)
    , m_actionHandler(std::move(actionHandler))
    , m_icon(icon)
{
}

/// @brief 更新勾选菜单项业务处理器跨帧状态。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给 action handler。
void MainMenuToggleItem::update(MainMenuContext& context)
{
    if ( m_actionHandler ) {
        m_actionHandler->update(context);
    }
}

/// @brief 绘制勾选菜单项并在状态变化时执行自身 action handler。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在所属菜单展开时执行。
void MainMenuToggleItem::render(MainMenuContext& context)
{
    bool* value = m_actionHandler ? m_actionHandler->value(context) : nullptr;
    const bool enabled = value && m_actionHandler->isEnabled(context);
    const bool clicked =
        m_icon ? renderMainMenuIconItem(
                     m_icon, resolveLabel(), nullptr, enabled, value && *value)
               : ::MMM::UI::FeedbackMenuItem(
                     resolveLabel(), nullptr, value, enabled);
    if ( clicked && value ) {
        if ( m_icon ) {
            *value = !*value;
        }
        m_actionHandler->execute(context, MainMenuItemActivation{});
    }
}

/// @brief 转发快捷键消费给勾选业务处理器。
/// @param context 单帧主菜单上下文。
/// @return 业务处理器消费快捷键时返回 true。
/// @warning UI 热路径：每帧执行；仅转发给 action handler。
bool MainMenuToggleItem::handleShortcut(MainMenuContext& context)
{
    if ( !m_actionHandler || !m_actionHandler->isEnabled(context) ) {
        return false;
    }
    return m_actionHandler->handleShortcut(context);
}

/// @brief 渲染勾选菜单项业务处理器延迟窗口。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给 action handler。
void MainMenuToggleItem::renderDeferred(MainMenuContext& context)
{
    if ( m_actionHandler ) {
        m_actionHandler->renderDeferred(context);
    }
}

/// @brief 获取当前帧的显示文本。
/// @return 当前帧应显示的菜单项文本。
const char* MainMenuToggleItem::resolveLabel() const
{
    if ( m_textKind == MainMenuItemTextKind::Literal ) {
        return m_label.c_str();
    }
    return TR(m_label.c_str()).data();
}

}  // namespace MMM::UI
