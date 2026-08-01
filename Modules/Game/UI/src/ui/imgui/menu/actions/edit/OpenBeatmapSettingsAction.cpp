#include "event/ui/UISettingsTabEvent.h"
#include "ui/UIManager.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"

namespace MMM::UI
{
namespace
{
/// @brief 打开谱面设置动作。
class OpenBeatmapSettingsAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 打开设置窗口的谱面页。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        if ( context.sourceManager ) {
            context.sourceManager->openSettingsWindow(
                Event::SettingsTab::Beatmap);
        }
    }
};
}  // namespace

/// @brief 创建打开谱面设置动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenBeatmapSettingsAction()
{
    return std::make_unique<OpenBeatmapSettingsAction>();
}

}  // namespace MMM::UI
