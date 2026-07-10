#include "logic/EditorEngine.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 播放暂停切换动作。
class TogglePlaybackAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 根据当前播放状态返回播放或暂停图标。
    const char* icon(const MainMenuContext& context,
                     const char*            fallbackIcon) const override
    {
        (void)context;
        (void)fallbackIcon;
        return Logic::EditorEngine::instance().isPlaybackPlaying()
                   ? ICON_MMM_PAUSE
                   : ICON_MMM_PLAY;
    }

    /// @brief 切换播放状态。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        const bool playing =
            Logic::EditorEngine::instance().isPlaybackPlaying();
        MenuUtil::dispatchCommand(Logic::CmdSetPlayState{ !playing });
    }

    /// @brief 消费空格播放暂停快捷键。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取输入状态和当前交互状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        ImGuiIO& io = ImGui::GetIO();
        if ( !ImGui::IsKeyPressed(ImGuiKey_Space, false) ) return false;

        ImGuiContext* imguiContext = ImGui::GetCurrentContext();
        const bool    bpmToolFocused =
            imguiContext && imguiContext->NavWindow &&
            imguiContext->NavWindow->Name &&
            isBpmMeasurementToolStableWindowId(
                ShortcutUtils::stableWindowId(imguiContext->NavWindow->Name));
        if ( bpmToolFocused ) {
            auto* bpmTool =
                context.sourceManager
                    ? context.sourceManager->getView<BpmMeasurementToolView>(
                          "BpmMeasurementTool")
                    : nullptr;
            if ( !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && !io.KeyShift &&
                 !ImGui::IsAnyItemActive() && bpmTool ) {
                // BPM 窗口级空格键优先于当前导航控件，避免同一按键在本帧
                // 再次激活播放按钮或其它控件。
                imguiContext->NavActivateId = 0;
                bpmTool->togglePlaybackFromShortcut();
            }
            return true;
        }

        if ( io.KeyCtrl || io.KeyAlt || io.KeySuper ) return false;

        auto& engine = Logic::EditorEngine::instance();
        if ( ImGui::IsAnyItemActive() ) {
            const bool allowPlaybackToggle =
                (!io.KeyShift && (engine.isActiveSessionSelectingMarquee() ||
                                  engine.isActiveSessionDraggingNote())) ||
                (io.KeyShift && engine.isActiveSessionDrawingBrush());
            if ( !allowPlaybackToggle ) return false;
        } else if ( io.KeyShift ) {
            return false;
        }

        execute(context, MainMenuItemActivation{});
        return true;
    }
};
}  // namespace

/// @brief 创建播放暂停切换动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createTogglePlaybackAction()
{
    return std::make_unique<TogglePlaybackAction>();
}

}  // namespace MMM::UI
