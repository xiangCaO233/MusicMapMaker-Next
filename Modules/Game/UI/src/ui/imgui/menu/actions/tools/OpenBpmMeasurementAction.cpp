#include "config/skin/SkinConfig.h"
#include "logic/EditorEngine.h"
#include "ui/UIManager.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "ui/utils/UIWidgetUtils.h"
#include <memory>
#include <string>

namespace MMM::UI
{
namespace
{
/// @brief 打开 BPM 测量工具动作。
class OpenBpmMeasurementAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在已有项目时允许打开 BPM 测量工具。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return Logic::EditorEngine::instance().getCurrentProject() != nullptr;
    }

    /// @brief 注册并打开 BPM 测量工具视图。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        if ( !context.sourceManager ) return;

        std::string viewName = "BpmMeasurementTool";
        auto*       tool =
            context.sourceManager->getView<BpmMeasurementToolView>(viewName);
        const bool wasOpen = tool && tool->isOpen();
        if ( !tool ) {
            auto toolView = std::make_unique<BpmMeasurementToolView>(
                TR("ui.tools.bpm_measure").data());
            tool = toolView.get();
            context.sourceManager->registerView(viewName, std::move(toolView));
        }
        if ( tool ) {
            tool->openWithAudioTrack("");
            if ( !wasOpen ) {
                ::MMM::UI::PlayPopupOpenFeedback();
            }
        }
    }
};
}  // namespace

/// @brief 创建打开 BPM 测量工具动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenBpmMeasurementAction()
{
    return std::make_unique<OpenBpmMeasurementAction>();
}

}  // namespace MMM::UI
