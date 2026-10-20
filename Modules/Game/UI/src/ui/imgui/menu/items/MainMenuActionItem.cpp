#include "ui/imgui/menu/items/MainMenuActionItem.h"
#include "config/skin/SkinConfig.h"
#include "ui/UIManager.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/items/MainMenuItemUtils.h"
#include "ui/interaction/EditorInteraction.h"
#include "ui/walkthrough/WalkthroughSpotlight.h"
#include <utility>

namespace MMM::UI
{

/// @brief 构造普通主菜单项。
/// @param icon 菜单项默认图标文本，可为空。
/// @param label 菜单项文本或翻译键。
/// @param textKind 菜单项文本来源。
/// @param shortcut 菜单项默认快捷键提示，可为空。
/// @param actionHandler 菜单项业务处理器。
/// @param walkthroughTarget 与演练配置约定的可选语义目标 ID。
/// @note 图标与快捷键均为非拥有指针，调用方须保证其静态或更长生命周期。
/// @note 引导目标为空时保持原有菜单项路径，不访问 Spotlight。
/// @warning 构造只发生在菜单注册阶段，允许转移目标字符串和处理器所有权。
MainMenuActionItem::MainMenuActionItem(
    const char* icon, std::string label, MainMenuItemTextKind textKind,
    const char*                                 shortcut,
    std::unique_ptr<IMainMenuItemActionHandler> actionHandler,
    std::string                                 walkthroughTarget)
    : m_icon(icon)
    , m_label(std::move(label))
    , m_textKind(textKind)
    , m_shortcut(shortcut)
    , m_actionHandler(std::move(actionHandler))
    , m_walkthroughTarget(std::move(walkthroughTarget))
{
    // 动作借用只存在于菜单生命周期，视图关闭后不能再按 ID 调用。
    if ( m_actionHandler ) Interaction::registerAction(*m_actionHandler);
}

/// @brief 析构前撤销动作借用，防止方案访问已经关闭的菜单。
MainMenuActionItem::~MainMenuActionItem()
{
    if ( m_actionHandler ) Interaction::unregisterAction(*m_actionHandler);
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
/// @note 业务处理器可按当前上下文覆盖默认图标与快捷键提示。
/// @details 绘制只报告激活，业务动作统一经过操作方案而不是直接执行。
/// 快捷键提示也使用相同方案解释，用户重绑后不能继续显示旧构造字符串。
/// 未绑定的动作显示空提示，但菜单动作本身仍可按原权限执行。
/// 动作登记只覆盖菜单对象的寿命，销毁后不能再从新手势请求同一个观察指针。
/// 教程目标只在动作路径确实执行后完成，禁用入口不能伪造完成状态。
void MainMenuActionItem::render(MainMenuContext& context)
{
    // 隐藏高级入口时不生成菜单控件；延迟窗口仍由动作处理器负责收起。
    // 可见性与权限分开，其他禁用条目继续保留原有置灰反馈。
    if ( m_actionHandler && !m_actionHandler->isVisible(context) ) return;
    const bool enabled =
        !m_actionHandler || m_actionHandler->isEnabled(context);
    const char* icon =
        m_actionHandler ? m_actionHandler->icon(context, m_icon) : m_icon;
    const char* shortcut = m_actionHandler
                               ? m_actionHandler->shortcut(context, m_shortcut)
                               : m_shortcut;
    // 统一提示也经过方案映射，重绑定后不能继续显示构造时的硬编码文本。
    std::string resolvedShortcut;
    if ( m_actionHandler && !m_actionHandler->shortcutActionId().empty() ) {
        const auto id = m_actionHandler->shortcutActionId();
        if ( const auto* descriptor = Config::findEditorShortcut(id) ) {
            const auto& configured = Config::AppConfig::instance()
                                         .getEditorSettings()
                                         .shortcutConfig.*
                                     (descriptor->m_binding);
            resolvedShortcut       = ShortcutUtils::formatShortcut(
                Interaction::activeProfile().shortcut(id, configured));
            shortcut =
                resolvedShortcut.empty() ? nullptr : resolvedShortcut.c_str();
        }
    }
    // 先保存激活结果，再紧邻控件读取 LastItem 矩形供任意演练配置定位。
    const bool activated =
        renderMainMenuIconItem(icon, resolveLabel(), shortcut, enabled);
    if ( context.sourceManager && !m_walkthroughTarget.empty() )
        context.sourceManager->walkthroughSpotlight().reportLastItem(
            m_walkthroughTarget);
    // 无处理器的条目仍可绘制为静态项，但不会响应激活。
    if ( activated && m_actionHandler ) {
        const bool executed = Interaction::dispatchAction(
            *m_actionHandler, context, MainMenuItemActivation{});
        if ( executed && context.sourceManager && !m_walkthroughTarget.empty() )
            // 只有启用条目的处理器已执行，才确认对应菜单目标完成。
            context.sourceManager->walkthroughSpotlight().completeTarget(
                m_walkthroughTarget, true);
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
/// @warning UI 热路径：翻译结果只保证当前翻译存储生命周期内有效。
const char* MainMenuActionItem::resolveLabel() const
{
    if ( m_textKind == MainMenuItemTextKind::Literal ) {
        return m_label.c_str();
    }
    return TR(m_label.c_str()).data();
}

}  // namespace MMM::UI
