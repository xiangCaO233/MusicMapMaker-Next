#include "config/AppConfig.h"
#include "ui/imgui/menu/actions/MainMenuViewActions.h"

namespace MMM::UI
{
namespace
{
/// @brief 固定工具窗口开关动作。
/// @details 绑定固定工具窗口布局偏好；菜单项只改变配置，不直接操纵停靠节点或
/// 当前窗口生命周期。
class FixedToolWindowToggleAction final
    : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取固定工具窗口设置。
    /// @param context 统一菜单上下文，本动作无需读取。
    /// @return EditorSettings::fixedToolWindow 的稳定地址。
    /// @warning UI 热路径：只提供状态绑定，不执行布局操作。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .fixedToolWindow;
    }

    /// @brief 状态变化后保存编辑器设置。
    /// @param context 统一菜单上下文。
    /// @param activation 激活来源；不改变保存策略。
    /// @note 布局代码在后续 UI 更新读取新值，本函数仅持久化。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        Config::AppConfig::instance().save();
    }
};
}  // namespace

/// @brief 创建固定工具窗口开关处理器。
/// @return 独占所有权的处理器实例。
/// @note 处理器不直接重建停靠布局。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createFixedToolWindowToggleAction()
{
    return std::make_unique<FixedToolWindowToggleAction>();
}

}  // namespace MMM::UI
