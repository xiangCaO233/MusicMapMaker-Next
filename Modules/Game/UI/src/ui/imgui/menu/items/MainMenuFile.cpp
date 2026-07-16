#include "ui/imgui/menu/items/MainMenuFile.h"
#include "config/skin/SkinConfig.h"
#include "ui/Icons.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"
#include "ui/imgui/menu/items/MainMenuActionItem.h"
#include "ui/imgui/menu/items/MainMenuRecentProjectsItem.h"
#include "ui/imgui/menu/items/MainMenuSeparatorItem.h"
#include <memory>
#include <utility>

namespace MMM::UI
{

/// @brief 构造文件菜单并注册默认菜单项。
MainMenuFile::MainMenuFile()
{
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_BOOK,
        "ui.file.new_pro",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+Shift+N",
        createOpenNewProjectWizardAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_FILE,
        "ui.file.new_map",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+N",
        createOpenNewBeatmapWizardAction()));
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_FOLDER_OPEN,
        "ui.file.open_pro",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+O",
        createOpenProjectAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_MUSIC,
        "ui.audio_manager.import_audio",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+I",
        createOpenAudioImportAction()));
    registerItem(std::make_unique<MainMenuRecentProjectsItem>(
        createOpenRecentProjectAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_FOLDER_OPEN,
        "ui.file.open_project_directory",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenProjectDirectoryAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_CLOSE,
        "ui.file.close_pro",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createCloseProjectAction()));
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_SAVE,
        "ui.file.save",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+S",
        createSaveBeatmapAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_SAVE,
        "ui.file.save_as",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+Shift+S",
        createSaveBeatmapAsAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_PACK,
        "ui.file.pack",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createPackBeatmapAction()));
}

/// @brief 获取文件菜单标识。
/// @return 文件菜单标识。
MainMenuId MainMenuFile::id() const
{
    return MainMenuId::File;
}

/// @brief 获取文件菜单显示文本。
/// @param context 单帧主菜单上下文。
/// @return 当前语言下的文件菜单文本。
const char* MainMenuFile::label(const MainMenuContext& context) const
{
    (void)context;
    return TR("ui.file");
}

/// @brief 遍历更新文件菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
void MainMenuFile::update(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->update(context);
    }
}

/// @brief 遍历文件菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在文件菜单展开时执行。
void MainMenuFile::render(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->render(context);
    }
}

/// @brief 遍历文件菜单项消费快捷键。
/// @param context 单帧主菜单上下文。
/// @return 有菜单项消费快捷键时返回 true。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
bool MainMenuFile::handleShortcut(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item && item->handleShortcut(context) ) return true;
    }
    return false;
}

/// @brief 遍历渲染文件菜单项的延迟窗口。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
void MainMenuFile::renderDeferred(MainMenuContext& context)
{
    for ( auto& item : m_items ) {
        if ( item ) item->renderDeferred(context);
    }
}

/// @brief 注册文件菜单项。
/// @param item 待注册菜单项。
void MainMenuFile::registerItem(std::unique_ptr<IMainMenuItem> item)
{
    if ( item ) {
        m_items.push_back(std::move(item));
    }
}

}  // namespace MMM::UI
