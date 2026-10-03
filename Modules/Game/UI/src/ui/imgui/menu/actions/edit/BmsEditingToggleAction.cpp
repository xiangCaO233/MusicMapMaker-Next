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
    /// @brief BMS 子开关只有专业模式开启时可以操作。
    /// @param context 当前菜单上下文，不读取会话锁。
    /// @return 软件级专业模式开启时返回 true。
    /// @warning 菜单热路径只读取一个配置布尔值。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return Config::AppConfig::instance()
            .getEditorSettings()
            .professionalMode;
    }

    /// @brief 获取 BMS 编辑设置。
    /// @param context 单帧主菜单上下文。
    /// @return AppConfig 中持久化的 BMS 编辑开关地址。
    /// @warning UI 热路径：仅在编辑菜单展开时读取现有配置地址。
    /// @note 返回地址归配置单例所有，处理器不取得所有权。
    bool* value(MainMenuContext& context) override
    {
        // 禁用时显示实际关闭状态，同时保留配置中专业模式的 BMS 子偏好。
        if ( !isEnabled(context) ) return &m_disabledValue;
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
        if ( !isEnabled(context) ) return;
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

private:
    /// @brief 普通模式的菜单展示值，不参与配置序列化或能力计算。
    bool m_disabledValue{ false };
};
}  // namespace

/// @brief 创建 BMS 编辑开关处理器。
/// @return 新建的 BMS 编辑开关处理器。
/// @note 专业模式绑定 AppConfig；普通模式使用禁用展示值。
std::unique_ptr<IMainMenuToggleItemActionHandler> createBmsEditingToggleAction()
{
    return std::make_unique<BmsEditingToggleAction>();
}

}  // namespace MMM::UI
