#include "config/AppConfig.h"
#include "ui/imgui/menu/actions/MainMenuViewActions.h"

namespace MMM::UI
{
namespace
{
/// @brief 固定工具窗口开关动作。
class FixedToolWindowToggleAction final
    : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取固定工具窗口设置。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .fixedToolWindow;
    }

    /// @brief 状态变化后保存编辑器设置。
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
std::unique_ptr<IMainMenuToggleItemActionHandler>
createFixedToolWindowToggleAction()
{
    return std::make_unique<FixedToolWindowToggleAction>();
}

}  // namespace MMM::UI
