#include "ui/UIManager.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuHelpActions.h"
#include <memory>

namespace MMM::UI
{
namespace
{
/// @brief 帮助菜单只请求显示窗口，不持有演练进度。
class OpenWelcomeAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 下一帧安全打开欢迎页的主题目录。
    void execute(MainMenuContext& context,
                 const MainMenuItemActivation&) override
    {
        if ( context.sourceManager ) context.sourceManager->openWelcome();
    }
};
}  // namespace
std::unique_ptr<IMainMenuItemActionHandler> createOpenWelcomeAction()
{
    return std::make_unique<OpenWelcomeAction>();
}
}  // namespace MMM::UI
