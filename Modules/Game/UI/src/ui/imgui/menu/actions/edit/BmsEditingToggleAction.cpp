#include "config/AppConfig.h"
#include "ui/IEditorApplicationService.h"
#include "ui/UIManager.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"

namespace MMM::UI
{
namespace
{
/// @brief BMS 编辑开关动作。
class BmsEditingToggleAction final : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取 BMS 编辑设置。
    /// @param context 单帧主菜单上下文。
    /// @return AppConfig 中持久化的 BMS 编辑开关地址。
    /// @warning UI 热路径：仅在编辑菜单展开时读取现有配置地址。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .enableBmsEditing;
    }

    /// @brief 状态变化后同步逻辑线程并保存编辑器设置。
    /// @param context 单帧主菜单上下文。
    /// @param activation 菜单项激活载荷。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        auto& appConfig = Config::AppConfig::instance();
        if ( context.sourceManager ) {
            if ( auto* service =
                     context.sourceManager->getEditorApplicationService() ) {
                service->updateEditorConfig(appConfig.getEditorConfig());
            }
        }
        appConfig.save();
    }
};
}  // namespace

/// @brief 创建 BMS 编辑开关处理器。
/// @return 新建的 BMS 编辑开关处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler> createBmsEditingToggleAction()
{
    return std::make_unique<BmsEditingToggleAction>();
}

}  // namespace MMM::UI
