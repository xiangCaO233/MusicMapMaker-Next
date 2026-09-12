#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/status/IStatusMessageSink.h"

#include "config/skin/SkinConfig.h"
#include "config/skin/translation/TranslationFormat.h"
#include "graphic/imguivk/VKContext.h"
#include "graphic/theme/ImGuiThemeRegistry.h"

#include <memory>
#include <string>

namespace MMM::UI
{
namespace
{

/// @brief 删除已载入自定义实例并重新扫描用户插件目录。
/// @details
/// 动作在显式触发时访问图形上下文，并将完整或部分成功映射为状态栏消息。
class ReloadPluginsAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 执行低频插件重载并发布结果提示。
    /// @param context 单帧主菜单上下文。
    /// @param activation 菜单项激活载荷。
    /// @note 插件重载属于低频管理操作，不应从逐帧路径自动调用。
    /// @warning 重载会替换插件实例，调用期间不得保留旧主题对象引用。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        // 图形上下文可能尚未建立；此时只能反馈不可用，不能尝试访问注册表。
        auto graphicContext = Graphic::VKContext::get();
        if ( !graphicContext ) {
            context.statusMessageSink.showStatusMessage(
                TR("ui.tools.reload_plugins.unavailable").data(), 4.0f);
            return;
        }

        // 由图形层统一执行卸载、扫描与重建，菜单层只消费汇总结果。
        const Graphic::ThemePluginReloadResult result =
            graphicContext->get().reloadPlugins();
        // 汇总结果按值保留到消息构造结束，避免引用重载过程中的临时状态。
        std::string message;
        if ( result.success() ) {
            // 无错误时突出本轮成功加载的主题数量。
            message = TR_FMT("ui.tools.reload_plugins.success",
                             result.loadedThemeCount);
        } else {
            // 部分失败仍保留已加载主题，并向用户报告错误条目数量。
            message = TR_FMT("ui.tools.reload_plugins.partial",
                             result.loadedThemeCount,
                             result.errors.size());
        }
        // 错误详情由插件管理界面负责呈现，此处只给出适合状态栏的摘要。
        // 统一经状态消息接口展示，避免菜单动作直接管理提示窗口生命周期。
        context.statusMessageSink.showStatusMessage(std::move(message), 4.0f);
    }
};

}  // namespace

/// @brief 创建插件重载动作处理器。
/// @return 独占所有权的无状态处理器。
/// @note 图形上下文仅在动作执行时获取，不被处理器长期持有。
/// @warning 调用方应只将该动作绑定到显式低频入口。
/// @warning 处理器应在图形上下文所属 UI 线程执行。
std::unique_ptr<IMainMenuItemActionHandler> createReloadPluginsAction()
{
    return std::make_unique<ReloadPluginsAction>();
}

}  // namespace MMM::UI
