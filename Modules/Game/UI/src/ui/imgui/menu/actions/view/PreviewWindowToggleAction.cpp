#include "config/AppConfig.h"
#include "ui/imgui/menu/actions/MainMenuViewActions.h"

namespace MMM::UI
{
namespace
{
/// @brief 预览窗口显示开关动作。
/// @details 复用软件配置中的可见性值，确保查看菜单、停靠窗口和下次启动状态
/// 共享同一事实来源。
class PreviewWindowToggleAction final : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取预览窗口显示设置。
    /// @param context 统一菜单上下文，本动作无需读取。
    /// @return EditorSettings::showPreviewWindow 的稳定地址。
    /// @warning UI 热路径：仅查询引用，不触发窗口重建。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .showPreviewWindow;
    }

    /// @brief 状态变化后保存编辑器设置。
    /// @param context 统一菜单上下文。
    /// @param activation 激活来源；本动作不区分来源。
    /// @note 可见性已经由菜单控件修改，此处只保存最新 AppConfig。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        Config::AppConfig::instance().save();
    }
};
}  // namespace

/// @brief 创建预览窗口显示开关处理器。
/// @return 独占所有权的处理器实例。
/// @note 处理器不拥有预览窗口。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createPreviewWindowToggleAction()
{
    return std::make_unique<PreviewWindowToggleAction>();
}

}  // namespace MMM::UI
