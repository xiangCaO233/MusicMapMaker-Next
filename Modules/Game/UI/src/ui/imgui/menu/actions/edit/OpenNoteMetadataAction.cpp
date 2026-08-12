#include "MetadataEditorWindowRenderers.h"
#include "logic/EditorEngine.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/utils/UIWidgetUtils.h"

namespace MMM::UI
{
namespace
{
/// @brief 打开选中谱面物件属性编辑器动作。
class OpenNoteMetadataAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在当前会话存在选中玩家物件或自动采样时允许打开。
    /// @warning UI 热路径：菜单每帧检查；只读取常量级选择索引，不遍历 ECS。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return Logic::EditorEngine::instance().hasActiveChartObjectSelection();
    }

    /// @brief 打开选中谱面物件属性编辑器窗口。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
        if ( !m_showWindow ) {
            ::MMM::UI::PlayPopupOpenFeedback();
        }
        m_showWindow = true;
    }

    /// @brief 渲染选中音符元数据编辑窗口。
    /// @param context 单帧主菜单上下文。
    /// @warning UI 热路径：每帧执行；仅在窗口打开时访问选中音符元数据。
    void renderDeferred(MainMenuContext& context) override
    {
        (void)context;
        renderNoteMetadataEditorWindow(m_showWindow);
    }

private:
    /// @brief 是否显示选中音符元数据编辑窗口。
    bool m_showWindow = false;
};
}  // namespace

/// @brief 创建打开音符元数据编辑器动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenNoteMetadataAction()
{
    return std::make_unique<OpenNoteMetadataAction>();
}

}  // namespace MMM::UI
