#include "config/Utf8Path.h"
#include "event/core/EventBus.h"
#include "event/ui/menu/OpenProjectEvent.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"

namespace MMM::UI
{
namespace
{
/// @brief 打开最近项目动作。
class OpenRecentProjectAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 按激活载荷中的路径发布打开项目事件。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        Event::OpenProjectEvent ev;
        ev.m_projectPath = Config::utf8ToPath(activation.textPayload);
        Event::EventBus::instance().publish(ev);
    }
};
}  // namespace

/// @brief 创建打开最近项目的菜单项业务处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenRecentProjectAction()
{
    return std::make_unique<OpenRecentProjectAction>();
}

}  // namespace MMM::UI
