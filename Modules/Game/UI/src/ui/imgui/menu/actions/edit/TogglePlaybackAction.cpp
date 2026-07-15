#include "logic/EditorEngine.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/actions/edit/PlaybackShortcutRouting.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>

namespace MMM::UI
{
namespace
{
/// @brief 判断 BPM 测量工具根窗口或其任意子窗口是否拥有键盘焦点。
/// @param context 当前 ImGui 上下文。
/// @return BPM 测量工具窗口层级内拥有焦点时返回 true。
/// @warning UI 热路径：空格按下时只沿当前焦点窗口的父级链执行短字符串比较。
bool isBpmMeasurementToolFocused(const ImGuiContext* context)
{
    const ImGuiWindow* window = context ? context->NavWindow : nullptr;
    while ( window ) {
        if ( window->Name &&
             isBpmMeasurementToolStableWindowId(
                 ShortcutUtils::stableWindowId(window->Name)) ) {
            return true;
        }
        window = window->ParentWindow;
    }
    return false;
}

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
        const bool bpmToolFocused  = isBpmMeasurementToolFocused(imguiContext);
        auto*      bpmTool =
            bpmToolFocused && context.sourceManager
                ? context.sourceManager->getView<BpmMeasurementToolView>(
                      "BpmMeasurementTool")
                : nullptr;
        const bool hasModifier =
            io.KeyCtrl || io.KeyAlt || io.KeySuper || io.KeyShift;
        const BpmSpaceShortcutDisposition bpmDisposition =
            resolveBpmSpaceShortcutDisposition(bpmToolFocused,
                                               hasModifier,
                                               io.WantTextInput,
                                               bpmTool != nullptr);
        if ( bpmDisposition != BpmSpaceShortcutDisposition::NotOwned ) {
            // BPM 窗口层级拥有空格键时禁止导航控件再次激活，也禁止事件
            // 穿透至背后的谱面编辑器。
            if ( imguiContext ) {
                imguiContext->NavActivateId = 0;
            }
            if ( bpmDisposition == BpmSpaceShortcutDisposition::ToggleTool ) {
                bpmTool->togglePlaybackFromShortcut();
            }
            return true;
        }

        if ( io.KeyCtrl || io.KeyAlt || io.KeySuper ) return false;

        auto& engine = Logic::EditorEngine::instance();
        if ( ImGui::IsAnyItemActive() ) {
            const bool timelineMarqueeSelecting =
                context.sourceManager &&
                context.sourceManager->isTimelineTimingMarqueeSelecting();
            const bool allowPlaybackToggle =
                shouldAllowPlaybackToggleWhileItemActive(
                    io.KeyShift,
                    engine.isActiveSessionSelectingMarquee(),
                    engine.isActiveSessionDraggingNote(),
                    engine.isActiveSessionDrawingBrush(),
                    timelineMarqueeSelecting);
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
