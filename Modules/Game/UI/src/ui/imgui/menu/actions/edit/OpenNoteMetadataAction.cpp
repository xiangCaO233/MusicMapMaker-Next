#include "MetadataEditorWindowRenderers.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/session/context/SessionContext.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include <mutex>

namespace MMM::UI
{
namespace
{
/// @brief 打开选中音符元数据编辑器动作。
class OpenNoteMetadataAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 仅在当前会话存在选中音符时允许打开。
    bool isEnabled(const MainMenuContext& context) const override
    {
        (void)context;
        bool  hasSelection = false;
        auto& engine       = Logic::EditorEngine::instance();
        std::lock_guard<std::recursive_mutex> sessionLock(
            engine.getSessionMutex());
        auto session = engine.getActiveSession();
        if ( session ) {
            auto selView =
                session->getContext()
                    .noteRegistry.view<const Logic::InteractionComponent>();
            for ( auto e : selView ) {
                if ( selView.get<const Logic::InteractionComponent>(e)
                         .isSelected ) {
                    hasSelection = true;
                    break;
                }
            }
        }
        return hasSelection;
    }

    /// @brief 打开选中音符元数据编辑器窗口。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)context;
        (void)activation;
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
