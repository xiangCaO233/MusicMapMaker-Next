#include "ui/imgui/menu/items/MainMenuViewMenu.h"
#include "config/skin/SkinConfig.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuViewActions.h"
#include "ui/imgui/menu/items/MainMenuSeparatorItem.h"
#include "ui/imgui/menu/items/MainMenuToggleItem.h"
#include "ui/imgui/menu/items/MainMenuToolbarVisibilityItem.h"
#include <memory>
#include <utility>

namespace MMM::UI
{

/// @brief 构造视图菜单并注册默认菜单项。
/// @details 注册顺序先提供主要窗口开关，再提供工具栏与标签显示配置。
MainMenuViewMenu::MainMenuViewMenu()
{
    // 主要编辑窗口开关集中排列，便于快速恢复工作区视图。
    registerItem(std::make_unique<MainMenuToggleItem>(
        "ui.view.timeline",
        MainMenuItemTextKind::TranslationKey,
        createTimelineWindowToggleAction()));
    registerItem(std::make_unique<MainMenuToggleItem>(
        "ui.view.preview",
        MainMenuItemTextKind::TranslationKey,
        createPreviewWindowToggleAction()));
    registerItem(std::make_unique<MainMenuToggleItem>(
        "ui.view.annotation_details",
        MainMenuItemTextKind::TranslationKey,
        createAnnotationDetailsToggleAction()));
    // 分隔窗口可见性与工具栏外观配置两个职责区域。
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    // 工具栏按钮细分配置由专用子菜单项统一承载。
    registerItem(std::make_unique<MainMenuToolbarVisibilityItem>());
    // 标签与固定窗口选项直接绑定持久化编辑器设置。
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
/// @note 标识用于 Alt 导航请求索引，必须保持稳定。
MainMenuId MainMenuViewMenu::id() const
{
    return MainMenuId::View;
}

/// @brief 获取视图菜单显示文本。
/// @param context 单帧主菜单上下文。
/// @return 当前语言下的视图菜单文本。
/// @warning 返回文本由翻译系统管理，不得由调用方释放。
const char* MainMenuViewMenu::label(const MainMenuContext& context) const
{
    (void)context;
    return TR("ui.view").data();
}

/// @brief 遍历更新视图菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 即使菜单未展开也需更新动作持有的跨帧状态。
void MainMenuViewMenu::update(MainMenuContext& context)
{
    // 容忍空槽位，保证单项注册失败不影响其余菜单状态更新。
    for ( auto& item : m_items ) {
        if ( item ) item->update(context);
    }
}

/// @brief 遍历视图菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在视图菜单展开时执行。
/// @note 绘制顺序严格遵循构造阶段的注册顺序。
void MainMenuViewMenu::render(MainMenuContext& context)
{
    // 各菜单项自行决定启用状态与当前勾选值。
    for ( auto& item : m_items ) {
        if ( item ) item->render(context);
    }
}

/// @brief 遍历视图菜单项消费快捷键。
/// @param context 单帧主菜单上下文。
/// @return 有菜单项消费快捷键时返回 true。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 首个消费快捷键的菜单项终止遍历，避免重复执行。
bool MainMenuViewMenu::handleShortcut(MainMenuContext& context)
{
    // 注册顺序同时定义潜在快捷键冲突的优先级。
    for ( auto& item : m_items ) {
        if ( item && item->handleShortcut(context) ) return true;
    }
    return false;
}

/// @brief 遍历渲染视图菜单项的延迟窗口。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 延迟内容在主菜单弹窗作用域结束后统一绘制。
void MainMenuViewMenu::renderDeferred(MainMenuContext& context)
{
    // 所有项目都获得延迟阶段，以支持动作持有的独立窗口。
    for ( auto& item : m_items ) {
        if ( item ) item->renderDeferred(context);
    }
}

/// @brief 注册视图菜单项。
/// @param item 待注册菜单项。
/// @note 空项被忽略，容器只保存有效独占所有权。
/// @warning 仅应在菜单初始化阶段调用，避免热路径重新分配。
void MainMenuViewMenu::registerItem(std::unique_ptr<IMainMenuItem> item)
{
    if ( item ) {
        // unique_ptr 转移后由视图菜单统一管理项目生命周期。
        m_items.push_back(std::move(item));
    }
}

}  // namespace MMM::UI
