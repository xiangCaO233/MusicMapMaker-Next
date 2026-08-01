#include "config/AppConfig.h"
#include "logic/EditorEngine.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"

namespace MMM::UI
{
namespace
{
/// @brief 折线编辑开关动作。
class PolylineEditingToggleAction final
    : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取折线编辑设置。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .enablePolylineEditing;
    }

    /// @brief 状态变化后同步逻辑线程并保存编辑器设置。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        auto& appConfig = Config::AppConfig::instance();
        Logic::EditorEngine::instance().setEditorConfig(
            appConfig.getEditorConfig());
        appConfig.save();
    }
};
}  // namespace

/// @brief 创建折线编辑开关处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createPolylineEditingToggleAction()
{
    return std::make_unique<PolylineEditingToggleAction>();
}

}  // namespace MMM::UI
