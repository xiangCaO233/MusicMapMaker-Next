#include "../edit/MetadataEditorWindowRenderers.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"
#include "ui/utils/UIWidgetUtils.h"

namespace MMM::UI
{
namespace
{
/// @brief 切换谱面额外元数据编辑窗口动作。
/// @details 窗口可见性保存在处理器实例中，谱面内容仍由渲染器按帧读取。
class ToggleMetadataEditorWindowAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 切换谱面额外元数据编辑窗口打开状态。
    /// @param context 单帧主菜单上下文，本动作无需读取。
    /// @param activation 激活来源，不影响窗口切换语义。
    /// @note 仅从关闭切换到打开时播放弹窗反馈，关闭保持安静。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        // 在翻转状态前判断旧值，以免关闭窗口时误播打开反馈。
        if ( !m_showWindow ) {
            ::MMM::UI::PlayPopupOpenFeedback();
        }
        m_showWindow = !m_showWindow;
    }

    /// @brief 渲染谱面额外元数据编辑窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅在窗口打开时访问当前谱面元数据。
    void renderDeferred(MainMenuContext& context) override
    {
        (void)context;
        // 延后渲染可避免工具窗口嵌套在主菜单栏作用域内。
        renderMetadataEditorWindow(m_showWindow);
    }

private:
    /// @brief 是否显示谱面额外元数据编辑窗口。
    bool m_showWindow = false;
};
}  // namespace

/// @brief 创建切换谱面额外元数据编辑窗口动作处理器。
/// @return 独占所有权的窗口状态处理器。
/// @warning 处理器生命周期决定未持久化的窗口显示状态。
std::unique_ptr<IMainMenuItemActionHandler>
createToggleMetadataEditorWindowAction()
{
    return std::make_unique<ToggleMetadataEditorWindowAction>();
}

}  // namespace MMM::UI
