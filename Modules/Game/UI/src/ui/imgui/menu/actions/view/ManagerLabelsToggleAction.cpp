#include "config/AppConfig.h"
#include "ui/imgui/menu/actions/MainMenuViewActions.h"

namespace MMM::UI
{
namespace
{
/// @brief 管理器标签显示开关动作。
class ManagerLabelsToggleAction final : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取管理器标签显示设置。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .showManagerLabels;
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

/// @brief 创建管理器标签显示开关处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createManagerLabelsToggleAction()
{
    return std::make_unique<ManagerLabelsToggleAction>();
}

}  // namespace MMM::UI
