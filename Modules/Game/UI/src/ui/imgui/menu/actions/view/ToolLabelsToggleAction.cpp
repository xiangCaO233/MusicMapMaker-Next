#include "config/AppConfig.h"
#include "ui/imgui/menu/actions/MainMenuViewActions.h"

namespace MMM::UI
{
namespace
{
/// @brief 工具按钮文本显示开关动作。
class ToolLabelsToggleAction final : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取工具按钮文本显示设置。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .showToolLabels;
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

/// @brief 创建工具按钮文本显示开关处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler> createToolLabelsToggleAction()
{
    return std::make_unique<ToolLabelsToggleAction>();
}

}  // namespace MMM::UI
