#include "ui/imgui/menu/items/MainMenuHelp.h"
#include "config/skin/SkinConfig.h"
#include "ui/Icons.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuHelpActions.h"
#include "ui/imgui/menu/items/MainMenuActionItem.h"
#include "ui/imgui/menu/items/MainMenuSeparatorItem.h"
#include <memory>
#include <utility>

namespace MMM::UI
{

/// @brief 构造帮助菜单并注册默认菜单项。
/// @details 菜单按欢迎入口、更新、支持目录和版本信息分组。
MainMenuHelp::MainMenuHelp()
{
    // 欢迎页作为首项，提供新用户入口和基础导航。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_INFO_CIRCLE,
        "ui.welcome.title",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenWelcomeAction()));
    // 更新检查与欢迎页职责独立，使用分隔符形成低频维护区。
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_DOWNLOAD,
        "ui.help.check_update",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createCheckUpdateAction()));
    // 本地支持路径集中排列，便于排查配置、皮肤和插件问题。
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_COG,
        "ui.help.open_software_configuration",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenSoftwareConfigurationAction()));
    // 配置文件入口尝试定位具体文件，目录入口只打开目标文件夹。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_FOLDER_OPEN,
        "ui.help.open_skins_directory",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenSkinsDirectoryAction()));
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_FOLDER_OPEN,
        "ui.help.open_plugins_directory",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenPluginsDirectoryAction()));
    // 关于窗口独立成末尾信息区，避免与文件系统动作混淆。
    registerItem(std::make_unique<MainMenuSeparatorItem>());
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_INFO_CIRCLE,
        "ui.help.about",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createShowAboutAction()));
    // 关于动作负责自身模态窗口，帮助菜单不保存弹窗状态。
}

/// @brief 获取帮助菜单标识。
/// @return 帮助菜单标识。
/// @note 标识供一级菜单导航控制器映射固定请求槽位。
MainMenuId MainMenuHelp::id() const
{
    return MainMenuId::Help;
}

/// @brief 获取帮助菜单显示文本。
/// @param context 单帧主菜单上下文。
/// @return 当前语言下的帮助菜单文本。
/// @warning 返回值由翻译存储管理，只在其生命周期内有效。
const char* MainMenuHelp::label(const MainMenuContext& context) const
{
    (void)context;
    return TR("ui.help").data();
}

/// @brief 遍历更新帮助菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 更新阶段允许检查更新动作推进非阻塞状态机。
void MainMenuHelp::update(MainMenuContext& context)
{
    // 空项目被安全跳过，避免局部初始化失败中断整组更新。
    for ( auto& item : m_items ) {
        if ( item ) item->update(context);
    }
}

/// @brief 遍历帮助菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在帮助菜单展开时执行。
/// @note 项目顺序保持构造阶段定义的信息层次。
void MainMenuHelp::render(MainMenuContext& context)
{
    // 具体启用和反馈规则由各菜单项及动作处理器负责。
    for ( auto& item : m_items ) {
        if ( item ) item->render(context);
    }
}

/// @brief 遍历帮助菜单项消费快捷键。
/// @param context 单帧主菜单上下文。
/// @return 有菜单项消费快捷键时返回 true。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 首个消费结果立即返回，防止多个帮助动作共享输入。
bool MainMenuHelp::handleShortcut(MainMenuContext& context)
{
    // 注册顺序定义冲突时的处理优先级。
    for ( auto& item : m_items ) {
        if ( item && item->handleShortcut(context) ) return true;
    }
    return false;
}

/// @brief 遍历渲染帮助菜单项的延迟窗口。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 关于、欢迎与更新弹窗均在菜单栏作用域外绘制。
void MainMenuHelp::renderDeferred(MainMenuContext& context)
{
    // 即使菜单未展开也需推进已打开的独立窗口。
    for ( auto& item : m_items ) {
        if ( item ) item->renderDeferred(context);
    }
}

/// @brief 注册帮助菜单项。
/// @param item 待注册菜单项。
/// @note 空项目不会写入容器。
/// @warning 该入口用于构造期装配，不应在逐帧路径动态注册。
void MainMenuHelp::registerItem(std::unique_ptr<IMainMenuItem> item)
{
    if ( item ) {
        // 所有权转移后由帮助菜单负责销毁项目与动作处理器。
        m_items.push_back(std::move(item));
    }
}

}  // namespace MMM::UI
