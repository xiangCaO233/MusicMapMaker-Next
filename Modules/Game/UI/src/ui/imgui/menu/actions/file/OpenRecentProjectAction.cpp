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
/// @details 最近项目项把 UTF-8 路径放入激活载荷；动作统一转换为平台 path 并发布
/// OpenProjectEvent，不直接执行项目加载。
class OpenRecentProjectAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 按激活载荷中的路径发布打开项目事件。
    /// @param context 统一菜单上下文，本动作无需读取。
    /// @param activation textPayload 携带最近项目 UTF-8 路径。
    /// @note 事件总线负责后续关闭确认、异步加载和错误反馈。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        // 路径只在事件边界转换一次，保留非 ASCII 平台路径。
        Event::OpenProjectEvent ev;
        ev.m_projectPath = Config::utf8ToPath(activation.textPayload);
        Event::EventBus::instance().publish(ev);
    }
};
}  // namespace

/// @brief 创建打开最近项目的菜单项业务处理器。
/// @return 独占所有权的无状态处理器。
/// @warning 调用方必须保证激活载荷来自受控最近项目列表。
/// @note 处理器不保留路径，避免最近列表刷新后使用旧条目。
std::unique_ptr<IMainMenuItemActionHandler> createOpenRecentProjectAction()
{
    return std::make_unique<OpenRecentProjectAction>();
}

}  // namespace MMM::UI
