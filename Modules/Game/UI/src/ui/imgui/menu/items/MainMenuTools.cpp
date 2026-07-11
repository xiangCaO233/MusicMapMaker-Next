#include "ui/imgui/menu/items/MainMenuTools.h"
#include "config/skin/SkinConfig.h"
#include "ui/Icons.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/menu/items/MainMenuActionItem.h"
#include <memory>
#include <utility>

namespace MMM::UI
{

/// @brief 构造工具菜单并注册默认菜单项。
MainMenuTools::MainMenuTools()
{
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_MUSIC,
        "ui.tools.bpm_measure",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenBpmMeasurementAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_SELECT_ALL,
        "ui.tools.overlap_check",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createToggleOverlapCheckWindowAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_COG,
        "谱面额外元数据编辑",
        MainMenuItemTextKind::Literal,
        nullptr,
        createToggleMetadataEditorWindowAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_BARS,
        "数据来源替换工具",
        MainMenuItemTextKind::Literal,
        nullptr,
        createOpenDataSourceReplaceWindowAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_BARS,
        "ui.tools.format",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+F",
        createAlignSelectedToCommonBeatsAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_MUSIC,
        "谱面倍速制作",
        MainMenuItemTextKind::Literal,
        nullptr,
        createOpenBeatmapSpeedExportAction()));
}

/// @brief 获取工具菜单标识。
/// @return 工具菜单标识。
MainMenuId MainMenuTools::id() const
{
    return MainMenuId::Tools;
}

/// @brief 获取工具菜单显示文本。
/// @param context 单帧主菜单上下文。
/// @return 当前语言下的工具菜单文本。
const char* MainMenuTools::label(const MainMenuContext& context) const
{
    (void)context;
    return TR("ui.tools");
}

/// @brief 遍历更新工具菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
void MainMenuTools::update(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->update(context);
    }
}

/// @brief 遍历工具菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在工具菜单展开时执行。
void MainMenuTools::render(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->render(context);
    }
}

/// @brief 遍历工具菜单项消费快捷键。
/// @param context 单帧主菜单上下文。
/// @return 有菜单项消费快捷键时返回 true。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
bool MainMenuTools::handleShortcut(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item && item->handleShortcut(context) ) return true;
    }
    return false;
}

/// @brief 遍历渲染工具菜单项的延迟窗口。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
void MainMenuTools::renderDeferred(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->renderDeferred(context);
    }
}

/// @brief 注册工具菜单项。
/// @param item 待注册菜单项。
void MainMenuTools::registerItem(std::unique_ptr<IMainMenuItem> item)
{
    if ( item ) {
        m_items.push_back(std::move(item));
    }
}

}  // namespace MMM::UI
