#include "config/AppConfig.h"
#include "ui/IEditorApplicationService.h"
#include "ui/UIManager.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"

#include <memory>

namespace MMM::UI
{
namespace
{
/// @brief 统一切换时间线专业分轨与各主画布草稿区的全局专业模式。
class ProfessionalModeToggleAction final
    : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取全局专业模式设置。
    /// @param context 单帧主菜单上下文。
    /// @return AppConfig 中持久化的全局专业模式开关地址。
    /// @warning UI 热路径：仅在编辑菜单展开时读取现有配置地址。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .professionalMode;
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

/// @brief 创建全局专业模式开关处理器。
/// @return 新建的全局专业模式开关处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createProfessionalModeToggleAction()
{
    return std::make_unique<ProfessionalModeToggleAction>();
}

}  // namespace MMM::UI
