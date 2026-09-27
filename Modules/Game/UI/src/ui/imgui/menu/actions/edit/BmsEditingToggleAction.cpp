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
/// @details 设置地址直接绑定 AppConfig；切换后把完整 EditorConfig 推送给逻辑
/// 服务，使 BMS 编辑规则与 UI 勾选在同一帧对齐。
class BmsEditingToggleAction final : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取 BMS 编辑设置。
    /// @param context 单帧主菜单上下文。
    /// @return AppConfig 中持久化的 BMS 编辑开关地址。
    /// @warning UI 热路径：仅在编辑菜单展开时读取现有配置地址。
    /// @note 返回地址归配置单例所有，处理器不取得所有权。
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
    /// @note 缺少 SourceManager
    /// 或应用服务时仍保存配置，下一次初始化会读取新值。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        auto& appConfig = Config::AppConfig::instance();
        // 服务可用时先更新运行态，再持久化相同配置快照。
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
/// @note 处理器无独立状态，实际值始终来自 AppConfig。
std::unique_ptr<IMainMenuToggleItemActionHandler> createBmsEditingToggleAction()
{
    return std::make_unique<BmsEditingToggleAction>();
}

}  // namespace MMM::UI
