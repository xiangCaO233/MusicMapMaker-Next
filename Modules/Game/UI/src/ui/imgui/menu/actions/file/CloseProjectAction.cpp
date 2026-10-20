#include "event/core/EventBus.h"
#include "event/project/ProjectEvents.h"
#include "logic/EditorEngine.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"
#include <string_view>

namespace MMM::UI
{
namespace
{
/// @brief 关闭当前项目动作。
/// @details 根据项目是否为临时包选择不同确认事件，实际保存、提示和资源释放由
/// 项目生命周期处理器完成。
class CloseProjectAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 跨菜单和操作方案复用的稳定动作身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view operationId() const override
    {
        return "CloseProjectAction";
    }

    /// @brief 仅在已有项目时允许关闭项目。
    /// @param context 统一菜单上下文，本检查无需读取。
    /// @return EditorEngine 当前持有项目时返回 true。
    /// @warning UI 热路径：只读取项目观察指针。
    /// @note 临时项目和普通项目在可用性上采用相同判断。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return Logic::EditorEngine::instance().getCurrentProject() != nullptr;
    }

    /// @brief 根据临时项目状态发布关闭请求事件。
    /// @param context 统一菜单上下文。
    /// @param activation 激活来源，不影响关闭策略。
    /// @note 临时项目需要专用提示，避免把包内编辑误当普通项目保存。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        // 两个事件保持关闭策略分离，菜单层不直接销毁当前项目。
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
/// @return 独占所有权的无状态处理器。
/// @warning 处理器不拥有当前项目。
std::unique_ptr<IMainMenuItemActionHandler> createCloseProjectAction()
{
    return std::make_unique<CloseProjectAction>();
}

}  // namespace MMM::UI
