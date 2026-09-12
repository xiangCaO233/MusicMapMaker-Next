#include "config/AppConfig.h"
#include "ui/imgui/menu/actions/MainMenuViewActions.h"

namespace MMM::UI
{
namespace
{
/// @brief 时间线窗口显示开关动作。
/// @details 将菜单勾选直接绑定到
/// EditorSettings::showTimelineWindow，避免窗口关闭
/// 与菜单状态之间发生跨帧偏差。
class TimelineWindowToggleAction final : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取时间线窗口显示设置。
    /// @param context 统一菜单上下文，本动作无需读取。
    /// @return AppConfig 中时间线可见性成员地址。
    /// @warning UI 热路径：不得执行持久化或时间线扫描。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .showTimelineWindow;
    }

    /// @brief 状态变化后保存编辑器设置。
    /// @param context 统一菜单上下文。
    /// @param activation 激活来源；所有来源统一保存。
    /// @note 控件负责翻转布尔值，本函数不重复修改状态。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        Config::AppConfig::instance().save();
    }
};
}  // namespace

/// @brief 创建时间线窗口显示开关处理器。
/// @return 独占所有权的处理器实例。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createTimelineWindowToggleAction()
{
    return std::make_unique<TimelineWindowToggleAction>();
}

}  // namespace MMM::UI
