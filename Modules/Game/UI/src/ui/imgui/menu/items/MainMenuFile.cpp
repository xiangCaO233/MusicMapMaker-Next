#include "ui/imgui/menu/items/MainMenuFile.h"
#include "config/skin/SkinConfig.h"
#include "ui/Icons.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"
#include "ui/imgui/menu/items/MainMenuActionItem.h"
#include "ui/imgui/menu/items/MainMenuRecentProjectsItem.h"
#include "ui/imgui/menu/items/MainMenuSeparatorItem.h"
#include <memory>
#include <utility>

namespace MMM::UI
{

/// @brief 构造文件菜单并注册默认菜单项。
/// @details 菜单按新建、打开与关闭、保存与打包三个职责区域排列。
MainMenuFile::MainMenuFile()
{
    // 新建项目和新建谱面分别覆盖容器级与内容级创建流程。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_BOOK,
        "ui.file.new_pro",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+Shift+N",
        createOpenNewProjectWizardAction()));
    // 项目向导负责创建容器和首份基础配置。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_FILE,
        "ui.file.new_map",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+N",
        createOpenNewBeatmapWizardAction()));
    // 谱面向导只向现有项目添加新的可编辑谱面。
    // 分隔创建入口与已有项目的打开和导入入口。
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    // 项目打开与音频导入保留各自快捷键，避免入口职责混合。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_FOLDER_OPEN,
        "ui.file.open_pro",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+O",
        createOpenProjectAction()));
    // 音频导入保持为项目内容操作，不与目录选择器共享状态。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_MUSIC,
        "ui.audio_manager.import_audio",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+I",
        createOpenAudioImportAction()));
    // 最近项目项动态读取配置列表，不在构造器复制路径集合。
    registerItem(std::make_unique<MainMenuRecentProjectsItem>(
        createOpenRecentProjectAction()));
    // 目录定位和关闭项目只在有效项目上下文中启用。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_FOLDER_OPEN,
        "ui.file.open_project_directory",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenProjectDirectoryAction()));
    // 目录定位只打开当前项目位置，不改变会话或最近项目列表。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_CLOSE,
        "ui.file.close_pro",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createCloseProjectAction()));
    // 保存类动作独立成组，降低与打开入口误触的风险。
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    // 普通保存与另存为共享文件职责但拥有不同快捷键和处理器。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_SAVE,
        "ui.file.save",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+S",
        createSaveBeatmapAction()));
    // 普通保存优先复用现有目标路径，不弹出路径选择器。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_SAVE,
        "ui.file.save_as",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+Shift+S",
        createSaveBeatmapAsAction()));
    // 打包位于文件菜单末尾，表示其依赖已保存项目资源。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_PACK,
        "ui.file.pack",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createPackBeatmapAction()));
    // 构造完成后项目集合保持不变，后续仅更新各动作内部状态。
}

/// @brief 获取文件菜单标识。
/// @return 文件菜单标识。
/// @note 标识保持稳定，以便一级菜单导航定位请求槽位。
MainMenuId MainMenuFile::id() const
{
    return MainMenuId::File;
}

/// @brief 获取文件菜单显示文本。
/// @param context 单帧主菜单上下文。
/// @return 当前语言下的文件菜单文本。
/// @warning 返回指针由翻译系统管理，调用方不得释放或跨语言切换缓存。
const char* MainMenuFile::label(const MainMenuContext& context) const
{
    (void)context;
    return TR("ui.file").data();
}

/// @brief 遍历更新文件菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 更新阶段推进文件动作持有的对话框和异步结果状态。
void MainMenuFile::update(MainMenuContext& context)
{
    // 空槽位被忽略，避免局部注册失败阻塞其余文件动作。
    for ( auto& item : m_items ) {
        if ( item ) item->update(context);
    }
}

/// @brief 遍历文件菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在文件菜单展开时执行。
/// @note 菜单项按构造注册顺序呈现，以维持固定操作分组。
void MainMenuFile::render(MainMenuContext& context)
{
    // 具体启用条件与可见反馈由每个项目的动作处理器负责。
    for ( auto& item : m_items ) {
        if ( item ) item->render(context);
    }
}

/// @brief 遍历文件菜单项消费快捷键。
/// @param context 单帧主菜单上下文。
/// @return 有菜单项消费快捷键时返回 true。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 首个消费快捷键的项目终止遍历，确保一次输入只执行一个动作。
bool MainMenuFile::handleShortcut(MainMenuContext& context)
{
    // 注册顺序定义快捷键配置发生冲突时的稳定优先级。
    for ( auto& item : m_items ) {
        if ( item && item->handleShortcut(context) ) return true;
    }
    return false;
}

/// @brief 遍历渲染文件菜单项的延迟窗口。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 文件选择器、向导和打包窗口均在菜单栏作用域外绘制。
void MainMenuFile::renderDeferred(MainMenuContext& context)
{
    // 菜单关闭后仍需推进已打开的延迟窗口和结果处理。
    for ( auto& item : m_items ) {
        if ( item ) item->renderDeferred(context);
    }
}

/// @brief 注册文件菜单项。
/// @param item 待注册菜单项。
/// @note 空项目不写入容器，保持遍历集合只含有效所有权。
/// @warning 注册入口仅供初始化装配，逐帧路径不得扩容菜单集合。
void MainMenuFile::registerItem(std::unique_ptr<IMainMenuItem> item)
{
    if ( item ) {
        // unique_ptr 转移后由文件菜单统一管理项目生命周期。
        m_items.push_back(std::move(item));
    }
}

}  // namespace MMM::UI
