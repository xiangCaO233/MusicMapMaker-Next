#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuFileActions.h"
#include "ui/interaction/EditorInteraction.h"
#include <string_view>

#include "ui/UIManager.h"
#include "ui/imgui/manager/NewProjectWizard.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 打开新建项目向导动作。
/// @details 菜单栈内只设置 pending 标志，窗口查找和打开延迟到 renderDeferred，
/// 避免在 ImGui 菜单渲染过程中改变顶层窗口状态。
class OpenNewProjectWizardAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 跨菜单和操作方案复用的稳定动作身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view operationId() const override
    {
        return "OpenNewProjectWizardAction";
    }
    /// @brief 菜单提示与触发共用方案解析后的快捷键身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view shortcutActionId() const override { return "newProject"; }

    /// @brief 标记下一次延迟渲染打开新建项目向导。
    /// @param context 统一菜单上下文。
    /// @param activation 激活来源，不改变延迟语义。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        m_pendingOrigin = Event::ProjectOpenOrigin::CreateFileMenu;
    }

    /// @brief 消费 Ctrl+Shift+N 快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取 ImGui 按键状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        // Ctrl+Shift+N 与新建谱面的 Ctrl+N 明确区分。
        (void)context;
        if ( ShortcutUtils::isActionShortcutPressed("newProject", true) ) {
            m_pendingOrigin = Event::ProjectOpenOrigin::CreateShortcut;
            return true;
        }
        return false;
    }

    /// @brief 在菜单栏窗口外打开新建项目向导。
    /// @param context 提供可选 UIManager。
    /// @warning UI
    /// 热路径：每帧只检查入口枚举，实际打开仅由用户点击或快捷键触发。
    /// @note pendingOrigin 为 Unknown 时不查询视图注册表。
    void renderDeferred(MainMenuContext& context) override
    {
        // 服务暂不可用时保留 pending，后续帧仍可完成请求。
        if ( m_pendingOrigin == Event::ProjectOpenOrigin::Unknown ) return;
        if ( !context.sourceManager ) return;

        auto* wizard = context.sourceManager->getView<NewProjectWizard>(
            "NewProjectWizard");
        if ( wizard ) {
            wizard->open(m_pendingOrigin);
            // 只有实际找到并打开向导后才消费请求。
            m_pendingOrigin = Event::ProjectOpenOrigin::Unknown;
        }
    }

private:
    /// @brief 等待打开新建项目向导的入口；Unknown 表示没有请求。
    Event::ProjectOpenOrigin m_pendingOrigin{
        Event::ProjectOpenOrigin::Unknown
    };
};
}  // namespace

/// @brief 创建打开新建项目向导的菜单项业务处理器。
/// @return 独占所有权并持有延迟打开标志的处理器。
/// @warning 处理器不拥有 NewProjectWizard。
std::unique_ptr<IMainMenuItemActionHandler> createOpenNewProjectWizardAction()
{
    return std::make_unique<OpenNewProjectWizardAction>();
}

}  // namespace MMM::UI
