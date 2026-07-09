#include "../edit/MetadataEditorWindowRenderers.h"
#include "ui/imgui/menu/actions/MainMenuToolsActions.h"

namespace MMM::UI
{
namespace
{
/// @brief 切换谱面额外元数据编辑窗口动作。
class ToggleMetadataEditorWindowAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 切换谱面额外元数据编辑窗口打开状态。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        m_showWindow = !m_showWindow;
    }

    /// @brief 渲染谱面额外元数据编辑窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅在窗口打开时访问当前谱面元数据。
    void renderDeferred(MainMenuContext& context) override
    {
        (void)context;
        renderMetadataEditorWindow(m_showWindow);
    }

private:
    /// @brief 是否显示谱面额外元数据编辑窗口。
    bool m_showWindow = false;
};
}  // namespace

/// @brief 创建切换谱面额外元数据编辑窗口动作处理器。
std::unique_ptr<IMainMenuItemActionHandler>
createToggleMetadataEditorWindowAction()
{
    return std::make_unique<ToggleMetadataEditorWindowAction>();
}

}  // namespace MMM::UI
