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
/// @details 菜单只维护一个全局设置源，具体时间线和画布在收到 EditorConfig 后
/// 各自更新表现，避免多个窗口开关发生分歧。
class ProfessionalModeToggleAction final
    : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取全局专业模式设置。
    /// @param context 单帧主菜单上下文。
    /// @return AppConfig 中持久化的全局专业模式开关地址。
    /// @warning UI 热路径：仅在编辑菜单展开时读取现有配置地址。
    /// @note 返回地址归配置单例所有，处理器不缓存引用。
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
    /// @note 运行服务暂不可用时仍持久化设置，后续编辑器初始化会应用它。
    /// @warning 只在用户切换时执行配置保存，不属于每帧渲染路径。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        auto& appConfig = Config::AppConfig::instance();
        // 先向现有逻辑会话推送，再保存同一 AppConfig 状态供下次启动使用。
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
/// @note 处理器不缓存模式状态，避免与配置单例产生双份事实。
/// @warning 返回对象应由菜单注册表独占管理。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createProfessionalModeToggleAction()
{
    return std::make_unique<ProfessionalModeToggleAction>();
}

}  // namespace MMM::UI
