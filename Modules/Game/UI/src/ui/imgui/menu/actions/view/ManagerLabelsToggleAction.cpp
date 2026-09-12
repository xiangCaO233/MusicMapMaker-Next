#include "config/AppConfig.h"
#include "ui/imgui/menu/actions/MainMenuViewActions.h"

namespace MMM::UI
{
namespace
{
/// @brief 管理器标签显示开关动作。
/// @details 菜单与管理器区域共同读写
/// EditorSettings::showManagerLabels，避免维护 额外 UI 镜像状态。
class ManagerLabelsToggleAction final : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取管理器标签显示设置。
    /// @param context 统一菜单上下文，本设置无需读取。
    /// @return AppConfig 中稳定布尔成员的地址。
    /// @warning UI 热路径：不得在此保存配置或分配临时状态。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .showManagerLabels;
    }

    /// @brief 状态变化后保存编辑器设置。
    /// @param context 统一菜单上下文。
    /// @param activation 激活来源；不影响持久化结果。
    /// @note 菜单控件已先修改 value() 指向值，本函数只负责落盘。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        Config::AppConfig::instance().save();
    }
};
}  // namespace

/// @brief 创建管理器标签显示开关处理器。
/// @return 独占所有权的处理器实例。
/// @note 处理器不缓存配置值。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createManagerLabelsToggleAction()
{
    return std::make_unique<ManagerLabelsToggleAction>();
}

}  // namespace MMM::UI
