// 折线编辑开关读写具体设置，不能仅凭前向声明访问字段。
#include "config/AppConfig.h"
#include "config/EditorConfig.h"
#include "config/EditorSettings.h"
#include "ui/IEditorApplicationService.h"
#include "ui/UIManager.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include <string_view>

namespace MMM::UI
{
namespace
{
/// @brief 折线编辑开关动作。
/// @details 菜单绑定持久化设置；切换后将完整 EditorConfig 同步给活动逻辑服务，
/// 具体折线工具和交互系统在自身更新路径读取新配置。
class PolylineEditingToggleAction final
    : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 独立于翻译和键位的勾选动作身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view operationId() const override
    {
        return "PolylineEditingToggleAction";
    }

    /// @brief 获取折线编辑设置。
    /// @param context 统一菜单上下文，本动作无需读取。
    /// @return EditorSettings::enablePolylineEditing 的稳定地址。
    /// @warning UI 热路径：只返回配置成员，不执行保存或逻辑同步。
    /// @note 返回指针归配置单例所有，调用方不得释放。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .enablePolylineEditing;
    }

    /// @brief 状态变化后同步逻辑线程并保存编辑器设置。
    /// @param context 提供可选编辑器应用服务。
    /// @param activation 激活来源；菜单与快捷键使用相同设置结果。
    /// @note 服务不存在时仍保存配置，下次初始化会读取新值。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        // 菜单控件已翻转 value() 指向成员，此处读取同一配置快照进行同步。
        auto& appConfig = Config::AppConfig::instance();
        if ( context.sourceManager ) {
            if ( auto* service =
                     context.sourceManager->getEditorApplicationService() ) {
                service->updateEditorConfig(appConfig.getEditorConfig());
            }
        }
        // 持久化只发生在实际激活路径，不进入每帧 value() 查询。
        appConfig.save();
    }
};
}  // namespace

/// @brief 创建折线编辑开关处理器。
/// @return 独占所有权的无状态处理器。
/// @note 开关事实始终位于 AppConfig，处理器不缓存副本。
/// @warning 返回对象由菜单注册表独占管理。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createPolylineEditingToggleAction()
{
    return std::make_unique<PolylineEditingToggleAction>();
}

}  // namespace MMM::UI
