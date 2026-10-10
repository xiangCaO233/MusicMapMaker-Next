#include "ui/imgui/menu/items/MainMenuTools.h"
#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "ui/imgui/menu/actions/tools/BpmShortcutFocus.h"
#include "ui/imgui/menu/items/MainMenuActionItem.h"
#include <memory>
#include <utility>

namespace MMM::UI
{

/// @brief 构造工具菜单并注册默认菜单项。
/// @details 工具按音频分析、谱面检查编辑、导出与插件管理顺序排列。
MainMenuTools::MainMenuTools()
{
    // BPM 测量是独立音频分析视图入口。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_MUSIC,
        "ui.tools.bpm_measure",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenBpmMeasurementAction()));
    // 重叠检查与元数据编辑均作用于当前活动谱面。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_SELECT_ALL,
        "ui.tools.overlap_check",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createToggleOverlapCheckWindowAction()));
    // 重叠检查窗口负责呈现诊断结果，菜单只提供切换入口。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_COG,
        "ui.tools.metadata_editor",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createToggleMetadataEditorWindowAction()));
    // 数据源替换和节拍对齐属于批量编辑辅助工具。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_BARS,
        "ui.tools.data_source_replace",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenDataSourceReplaceWindowAction()));
    // 数据源替换具有独立延迟窗口，不在菜单展开作用域内渲染。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_BARS,
        "ui.tools.format",
        MainMenuItemTextKind::TranslationKey,
        "Ctrl+F",
        createAlignSelectedToCommonBeatsAction()));
    // 时间点模板作为单个工具入口；新建和选区捕获由工具内部管理。
    // 菜单只发出打开请求，避免展开工具菜单时读取个人库。
    // 关闭时间线主窗口不会隐藏这个独立模板工具。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_BARS,
        "ui.tools.timing_template",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenTimingTemplateAction()));
    // 倍速导出作为文件生成工具，保留独立动作处理器。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_MUSIC,
        "ui.tools.speed_export",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenBeatmapSpeedExportAction()));
    // 插件列表和重载入口相邻，形成插件管理区域。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_BARS,
        "ui.tools.plugin_list",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createOpenPluginListAction()));
    // 插件列表用于查看状态，重载动作负责显式刷新实例。
    registerItem(std::make_unique<MainMenuActionItem>(
        ICON_MMM_COG,
        "ui.tools.reload_plugins",
        MainMenuItemTextKind::TranslationKey,
        nullptr,
        createReloadPluginsAction()));
    // 初始化完成后菜单项集合固定，避免逐帧遍历期间失效。
}

/// @brief 获取工具菜单标识。
/// @return 工具菜单标识。
/// @note 稳定标识用于 Alt 导航与菜单打开请求路由。
MainMenuId MainMenuTools::id() const
{
    return MainMenuId::Tools;
}

/// @brief 获取工具菜单显示文本。
/// @param context 单帧主菜单上下文。
/// @return 当前语言下的工具菜单文本。
/// @warning 返回文本由翻译系统持有，调用方不得释放。
const char* MainMenuTools::label(const MainMenuContext& context) const
{
    (void)context;
    return TR("ui.tools").data();
}

/// @brief 遍历更新工具菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 更新阶段负责推进各工具动作持有的后台结果状态。
void MainMenuTools::update(MainMenuContext& context)
{
    // 保留空项防御，确保其余工具不受局部注册异常影响。
    for ( auto& item : m_items ) {
        if ( item ) item->update(context);
    }
}

/// @brief 遍历工具菜单项。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：仅在工具菜单展开时执行。
/// @note 工具项按照注册顺序绘制并解析各自启用状态。
void MainMenuTools::render(MainMenuContext& context)
{
    // 菜单容器只负责遍历，不介入具体工具业务。
    for ( auto& item : m_items ) {
        if ( item ) item->render(context);
    }
}

/// @brief 遍历工具菜单项消费快捷键。
/// @param context 单帧主菜单上下文。
/// @return 有菜单项消费快捷键时返回 true。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 首个命中项停止遍历，防止配置冲突导致重复执行。
/// @note BPM 工具添加段落绑定由设置页录制，仅视图聚焦时生效。
/// 工具动作先于菜单工具项遍历，以免已有工具项抢占相同绑定。
bool MainMenuTools::handleShortcut(MainMenuContext& context)
{
    // 视图专属绑定只有工具窗口聚焦时才占用，普通编辑器不受影响。
    // 配置引用仅在本帧使用，避免复制整份编辑设置。
    const auto& binding = Config::AppConfig::instance()
                              .getEditorSettings()
                              .shortcutConfig.addBpmSegmentAtViewCenter;
    if ( ShortcutUtils::isShortcutPressed(binding) &&
         isBpmMeasurementToolFocused(ImGui::GetCurrentContext()) ) {
        // 无实例时也消费工具专属组合键，不向背后的谱面发送编辑命令。
        // 文本输入和弹窗已由主菜单入口过滤，不会在输入框内添加段落。
        auto* tool =
            context.sourceManager
                ? context.sourceManager->getView<BpmMeasurementToolView>(
                      "BpmMeasurementTool")
                : nullptr;
        if ( tool ) tool->addSegmentAtViewCenterFromShortcut();
        return true;
    }
    // 注册顺序定义工具快捷键冲突时的稳定优先级。
    // 无工具专属命中时维持原有工具菜单路由顺序。
    for ( auto& item : m_items ) {
        if ( item && item->handleShortcut(context) ) return true;
    }
    return false;
}

/// @brief 遍历渲染工具菜单项的延迟窗口。
/// @param context 单帧主菜单上下文。
/// @warning UI 热路径：每帧执行；仅转发给菜单项。
/// @note 独立工具窗口必须在主菜单弹窗作用域外渲染。
void MainMenuTools::renderDeferred(MainMenuContext& context)
{
    // 每帧转发以维持已打开工具窗口及其非阻塞任务状态。
    for ( auto& item : m_items ) {
        if ( item ) item->renderDeferred(context);
    }
}

/// @brief 注册工具菜单项。
/// @param item 待注册菜单项。
/// @note 空项目被忽略，容器只接管有效 unique_ptr。
/// @warning 仅用于初始化期装配，热路径不得追加项目。
void MainMenuTools::registerItem(std::unique_ptr<IMainMenuItem> item)
{
    if ( item ) {
        // 转移所有权后，工具菜单统一管理项目生命周期。
        m_items.push_back(std::move(item));
    }
}

}  // namespace MMM::UI
