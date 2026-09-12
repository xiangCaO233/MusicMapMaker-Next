#include "config/AppConfig.h"
#include "ui/imgui/menu/actions/MainMenuViewActions.h"

#include <memory>

namespace MMM::UI
{
namespace
{
/// @brief 主画布批注详情卡片显示开关动作。
/// @details 菜单与画布批注渲染共享 showAnnotationDetails；关闭只影响详情卡片，
/// 不删除批注数据或选择状态。
class AnnotationDetailsToggleAction final
    : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 获取批注详情卡片显示设置。
    /// @param context 统一菜单上下文，本动作无需读取。
    /// @return EditorSettings 中批注详情可见性地址。
    /// @warning UI 热路径：不得遍历批注或触发谱面修改。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .showAnnotationDetails;
    }

    /// @brief 状态变化后保存编辑器设置。
    /// @param context 统一菜单上下文。
    /// @param activation 激活来源；鼠标和快捷键结果一致。
    /// @note 菜单控件已经更新目标布尔值，本函数只执行配置保存。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        Config::AppConfig::instance().save();
    }
};
}  // namespace

/// @brief 创建主画布批注详情显示开关处理器。
/// @return 独占所有权的处理器实例。
/// @note 处理器不拥有批注数据。
/// @warning 调用方必须维持菜单动作处理器的独占生命周期。
std::unique_ptr<IMainMenuToggleItemActionHandler>
createAnnotationDetailsToggleAction()
{
    return std::make_unique<AnnotationDetailsToggleAction>();
}

}  // namespace MMM::UI
