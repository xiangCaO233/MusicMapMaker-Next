#include "ui/imgui/menu/items/MainMenuEdit.h"
#include "config/skin/SkinConfig.h"
#include "ui/Icons.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/items/MainMenuActionItem.h"
#include "ui/imgui/menu/items/MainMenuSeparatorItem.h"
#include "ui/imgui/menu/items/MainMenuToggleItem.h"
#include <memory>
#include <utility>

namespace MMM::UI
{

/// @brief 构造编辑菜单并注册默认菜单项。
MainMenuEdit::MainMenuEdit()
{
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_UNDO,
        "ui.edit.undo",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+Z",
        createUndoAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_REDO,
        "ui.edit.redo",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+Y / Ctrl+Shift+Z",
        createRedoAction()));
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_SCISSORS,
        "ui.edit.cut",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+X",
        createCutAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_COPY,
        "ui.edit.copy",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+C",
        createCopyAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_PASTE,
        "ui.edit.paste",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+V",
        createPasteAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_MIRROR,
        "ui.edit.mirror_paste",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createMirrorPasteAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_MIRROR,
        "ui.edit.mirror",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createMirrorAction()));
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_SELECT_ALL,
        "ui.edit.select_all",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+A",
        createSelectAllAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_COG,
        "ui.edit.note_metadata",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenNoteMetadataAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_VOLUME_HIGH,
        "ui.edit.selected_volume",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createEditSelectedObjectVolumeAction()));
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    registerItem(std::make_unique<MainMenuToggleItem>(
        "ui.edit.bms_editing",
        MainMenuItemTextKind::TranslationKey,
        createBmsEditingToggleAction()));
    registerItem(std::make_unique<MainMenuToggleItem>(
        "ui.edit.polyline_editing",
        MainMenuItemTextKind::TranslationKey,
        createPolylineEditingToggleAction()));
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_PLAY,
        "ui.edit.play_pause",
        MainMenuItemTextKind::TranslationKey,
        "Space",
        createTogglePlaybackAction()));
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_FILE,
        "ui.edit.beatmap_settings",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenBeatmapSettingsAction()));
}

/// @brief 获取编辑菜单标识。
/// @return 编辑菜单标识。
MainMenuId MainMenuEdit::id() const
{
    return MainMenuId::Edit;
}

/// @brief 获取编辑菜单显示文本。
/// @param context 单帧主菜单上下文。
/// @return 当前语言下的编辑菜单文本。
const char* MainMenuEdit::label(const MainMenuContext& context) const
{
    (void)context;
    return TR("ui.edit").data();
}

/// @brief 遍历更新编辑菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
void MainMenuEdit::update(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->update(context);
    }
}

/// @brief 遍历编辑菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在编辑菜单展开时执行。
void MainMenuEdit::render(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->render(context);
    }
}

/// @brief 遍历编辑菜单项消费快捷键。
/// @param context 单帧主菜单上下文。
/// @return 有菜单项消费快捷键时返回 true。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
bool MainMenuEdit::handleShortcut(MainMenuContext& context)
{
    // 自定义镜像快捷键需要优先于 Ctrl+V/Ctrl+C 等默认编辑快捷键。
    constexpr std::size_t mirrorPasteItemIndex = 6;
    constexpr std::size_t mirrorItemIndex      = 7;
    IMainMenuItem* mirrorPasteItem = mirrorPasteItemIndex < m_items.size()
                                         ? m_items[mirrorPasteItemIndex].get()
                                         : nullptr;
    IMainMenuItem* mirrorItem      = mirrorItemIndex < m_items.size()
                                         ? m_items[mirrorItemIndex].get()
                                         : nullptr;

    if ( mirrorPasteItem && mirrorPasteItem->handleShortcut(context) ) {
        return true;
    }
    if ( mirrorItem && mirrorItem->handleShortcut(context) ) {
        return true;
    }

    for ( auto& item : m_items ) {
        if ( item.get() == mirrorPasteItem || item.get() == mirrorItem ) {
            continue;
        }
        if ( item && item->handleShortcut(context) ) return true;
    }
    return false;
}

/// @brief 遍历渲染编辑菜单项的延迟窗口。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
void MainMenuEdit::renderDeferred(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->renderDeferred(context);
    }
}

/// @brief 注册编辑菜单项。
/// @param item 待注册菜单项。
void MainMenuEdit::registerItem(std::unique_ptr<IMainMenuItem> item)
{
    if ( item ) {
        m_items.push_back(std::move(item));
    }
}

}  // namespace MMM::UI
