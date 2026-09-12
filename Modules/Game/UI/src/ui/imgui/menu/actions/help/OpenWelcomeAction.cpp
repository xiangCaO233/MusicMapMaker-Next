#include "ui/UIManager.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuHelpActions.h"
#include <memory>

namespace MMM::UI
{
namespace
{
/// @brief 帮助菜单只请求显示窗口，不持有演练进度。
/// @details 欢迎页自身管理主题目录和演练状态，菜单动作只通过 UIManager 请求
/// 下一帧显示，避免在菜单渲染栈内直接创建窗口。
class OpenWelcomeAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 下一帧安全打开欢迎页的主题目录。
    /// @param context 提供可选 UIManager 的单帧上下文。
    /// @param activation 激活载荷，欢迎页入口无需读取。
    /// @note UIManager 缺失时静默忽略，适配启动与关闭阶段。
    void execute(MainMenuContext& context,
                 const MainMenuItemActivation&) override
    {
        if ( context.sourceManager ) context.sourceManager->openWelcome();
    }
};
}  // namespace
/// @brief 创建欢迎页打开动作。
/// @return 独占所有权的无状态处理器。
/// @warning 返回对象不持有 UIManager，只在执行时读取上下文观察指针。
std::unique_ptr<IMainMenuItemActionHandler> createOpenWelcomeAction()
{
    return std::make_unique<OpenWelcomeAction>();
}
}  // namespace MMM::UI
