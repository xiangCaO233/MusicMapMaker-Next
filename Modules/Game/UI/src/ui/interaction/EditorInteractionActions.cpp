#include "ui/UIManager.h"
#include "ui/imgui/MainDockSpaceUI.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/interaction/EditorInteraction.h"

namespace MMM::UI::Interaction
{
/// @file EditorInteractionActions.cpp
/// @brief 把方案的动作请求接入主界面提供的反馈与业务上下文。
/// @details 与输入解释分开编译，纯输入测试无需构造完整主界面。
/// 动作实现仍属于既有菜单对象，桥不复制保存或向导状态。
/// 管理器由当前表面帧借用，不延长窗口生命周期。
/// @brief 通过主界面创建真实动作上下文，反馈和文件确认仍由现有业务负责。
bool executeAction(std::string_view id, UIManager* manager)
{
    return executeAction(
        id,
        manager,
        MainMenuItemActivation{
            .m_source = MainMenuActivationSource::InteractionProfile });
}
/// @brief 目标随激活值一起传入，不能从当前窗口标题推断播放或撤销归属。
/// @details 工具和谱面仍复用同一业务动作，区别只在其明确的上下文目标。
/// 未提供管理器时不访问主窗口，避免后台回调误调用前台业务。
/// 本桥只在输入激活时执行，不属于每帧查询或渲染资源路径。
bool executeAction(std::string_view id, UIManager* manager,
                   const MainMenuItemActivation& activation)
{
    if ( !manager ) return false;
    auto* dock = manager->getView<MainDockSpaceUI>("MainDockSpaceUI");
    return dock && dock->executeInteractionAction(id, manager, activation);
}

}  // namespace MMM::UI::Interaction
