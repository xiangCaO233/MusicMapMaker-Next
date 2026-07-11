#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "logic/EditorEngine.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"

namespace MMM::UI
{
namespace
{
/// @brief 关闭当前项目动作。
class CloseProjectAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在已有项目时允许关闭项目。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return Logic::EditorEngine::instance().getCurrentProject() != nullptr;
    }

    /// @brief 根据临时项目状态发布关闭请求事件。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        if ( Logic::EditorEngine::instance().isTemporaryProjectOpen() ) {
            Event::EventBus::instance().publish(
                Event::TemporaryProjectClosePromptRequestedEvent{});
        } else {
            Event::EventBus::instance().publish(
                Event::ProjectCloseRequestedEvent{});
        }
    }
};
}  // namespace

/// @brief 创建关闭当前项目的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createCloseProjectAction()
{
    return std::make_unique<CloseProjectAction>();
}

}  // namespace MMM::UI
