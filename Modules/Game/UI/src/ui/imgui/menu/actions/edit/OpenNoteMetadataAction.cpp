#include "MetadataEditorWindowRenderers.h"
#include "logic/EditorEngine.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/utils/UIWidgetUtils.h"
#include <string_view>

namespace MMM::UI
{
namespace
{
/// @brief 打开选中谱面物件属性编辑器动作。
/// @details 处理器持有窗口可见性，选择事实仍由 EditorEngine 提供；菜单关闭后
/// 延迟渲染阶段继续维护独立编辑窗口。
class OpenNoteMetadataAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 跨菜单和操作方案复用的稳定动作身份。
    /// @warning UI 热路径只返回静态身份，不构造字符串或执行配置查询。
    std::string_view operationId() const override
    {
        return "OpenNoteMetadataAction";
    }

    /// @brief 仅在当前会话存在选中玩家物件或自动采样时允许打开。
    /// @param context 统一菜单上下文，本检查无需读取。
    /// @return 存在可编辑图表对象选择时返回 true。
    /// @warning UI 热路径：菜单每帧检查；只读取常量级选择索引，不遍历 ECS。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        return Logic::EditorEngine::instance().hasActiveChartObjectSelection();
    }

    /// @brief 打开选中谱面物件属性编辑器窗口。
    /// @param context 统一菜单上下文。
    /// @param activation 激活来源，不改变窗口状态。
    /// @note 只在从关闭变为打开时播放反馈，重复激活仅保持窗口可见。
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
    /// @note 渲染器可通过引用把 m_showWindow 改回 false。
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
/// @return 独占所有权并持有窗口可见性的处理器。
std::unique_ptr<IMainMenuItemActionHandler> createOpenNoteMetadataAction()
{
    return std::make_unique<OpenNoteMetadataAction>();
}

}  // namespace MMM::UI
