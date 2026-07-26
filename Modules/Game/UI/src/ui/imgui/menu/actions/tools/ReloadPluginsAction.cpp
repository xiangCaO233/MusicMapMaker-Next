#include "ui/imgui/menu/actions/MainMenuToolsActions.h"

#include "config/skin/SkinConfig.h"
#include "config/skin/translation/Translation.h"
#include "graphic/imguivk/VKContext.h"

#include <memory>
#include <string>

namespace MMM::UI
{
namespace
{

/// @brief 删除已载入自定义实例并重新扫描用户插件目录。
class ReloadPluginsAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 执行低频插件重载并发布结果提示。
    /// @param context 单帧主菜单上下文。
    /// @param activation 菜单项激活载荷。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        auto graphicContext = Graphic::VKContext::get();
        if ( !graphicContext ) {
            context.statusMessageSink.showStatusMessage(
                TR("ui.tools.reload_plugins.unavailable").data(), 4.0f);
            return;
        }

        const Graphic::ThemePluginReloadResult result =
            graphicContext->get().reloadPlugins();
        std::string message;
        if ( result.success() ) {
            message = TR_FMT("ui.tools.reload_plugins.success",
                             result.loadedThemeCount);
        } else {
            message = TR_FMT("ui.tools.reload_plugins.partial",
                             result.loadedThemeCount,
                             result.errors.size());
        }
        context.statusMessageSink.showStatusMessage(std::move(message), 4.0f);
    }
};

}  // namespace

std::unique_ptr<IMainMenuItemActionHandler> createReloadPluginsAction()
{
    return std::make_unique<ReloadPluginsAction>();
}

}  // namespace MMM::UI
