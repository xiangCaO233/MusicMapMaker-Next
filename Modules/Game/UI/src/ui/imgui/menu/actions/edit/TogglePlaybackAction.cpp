#include "logic/EditorEngine.h"
#include "ui/Icons.h"
#include "ui/imgui/menu/MainMenuView.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
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
        if ( io.KeyCtrl || io.KeyAlt || io.KeySuper ) return false;
        if ( !ImGui::IsKeyPressed(ImGuiKey_Space, false) ) return false;

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
