#include "config/AppConfig.h"
#include "ui/imgui/menu/actions/MainMenuViewActions.h"

namespace MMM::UI
{
namespace
{
/// @brief 预览窗口显示开关动作。
class PreviewWindowToggleAction final : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取预览窗口显示设置。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .showPreviewWindow;
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

/// @brief 创建预览窗口显示开关处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createPreviewWindowToggleAction()
{
    return std::make_unique<PreviewWindowToggleAction>();
}

}  // namespace MMM::UI
