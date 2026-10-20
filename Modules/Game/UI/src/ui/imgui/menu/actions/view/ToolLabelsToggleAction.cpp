// 标签动作直接读写显示选项，完整设置只由实际操作端包含。
#include "config/AppConfig.h"
#include "config/EditorSettings.h"
#include "ui/imgui/menu/actions/MainMenuViewActions.h"
#include <string_view>

namespace MMM::UI
{
namespace
{
/// @brief 工具按钮文本显示开关动作。
/// @details 直接暴露 EditorSettings 中的布尔值，使菜单勾选与工具栏渲染读取同一
/// 状态；动作本身不缓存副本。
class ToolLabelsToggleAction final : public IMainMenuToggleItemActionHandler
{
public:
    /// @brief 独立于翻译和键位的勾选动作身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view operationId() const override
    {
        return "ToolLabelsToggleAction";
    }

    /// @brief 获取工具按钮文本显示设置。
    /// @param context 统一菜单上下文，本设置无需读取。
    /// @return AppConfig 内长期存活的设置地址。
    /// @warning UI 热路径：只返回观察地址，不执行持久化。
    bool* value(MainMenuContext& context) override
    {
        (void)context;
        return &Config::AppConfig::instance()
                    .getEditorSettings()
                    .showToolLabels;
    }

    /// @brief 状态变化后保存编辑器设置。
    /// @param context 统一菜单上下文，本动作无需读取。
    /// @param activation 激活来源，本开关对鼠标和快捷键采用相同保存语义。
    /// @warning 仅在实际切换时调用，文件写入不得放入 value() 热路径。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        Config::AppConfig::instance().save();
    }
};
}  // namespace

/// @brief 创建工具按钮文本显示开关处理器。
/// @return 独占所有权的无状态处理器。
std::unique_ptr<IMainMenuToggleItemActionHandler> createToolLabelsToggleAction()
{
    return std::make_unique<ToolLabelsToggleAction>();
}

}  // namespace MMM::UI
