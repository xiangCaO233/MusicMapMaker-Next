#include "config/AppConfig.h"
#include "logic/EditorEngine.h"
#include "ui/Icons.h"
#include "ui/UIManager.h"
#include "ui/imgui/ShortcutUtils.h"
#include "ui/imgui/menu/MainMenuTypes.h"
#include "ui/imgui/menu/actions/MainMenuEditActions.h"
#include "ui/imgui/menu/actions/edit/PlaybackShortcutRouting.h"
#include "ui/imgui/menu/actions/tools/BpmMeasurementToolView.h"
#include "ui/imgui/menu/utils/MenuUtil.h"
#include <imgui.h>
#include <string>

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

    /// @brief 获取用户配置的播放切换快捷键提示。
    const char* shortcut(const MainMenuContext& context,
                         const char*            fallbackShortcut) const override
    {
        (void)context;
        (void)fallbackShortcut;
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        m_shortcutBuffer =
            ShortcutUtils::formatShortcut(shortcutConfig.togglePlayback);
        return m_shortcutBuffer.empty() ? nullptr : m_shortcutBuffer.c_str();
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

    /// @brief 消费播放暂停快捷键，并保留 BPM 工具对空格键的专用路由。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取输入状态和当前交互状态。
    bool handleShortcut(MainMenuContext& context) override
    {
        ImGuiIO&      io           = ImGui::GetIO();
        ImGuiContext* imguiContext = ImGui::GetCurrentContext();
        if ( ImGui::IsKeyPressed(ImGuiKey_Space, false) ) {
            const bool bpmToolFocused =
                isBpmMeasurementToolFocused(imguiContext);
            auto* bpmTool =
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
                consumePlaybackShortcutNavigationActivation(imguiContext);
                if ( bpmDisposition ==
                     BpmSpaceShortcutDisposition::ToggleTool ) {
                    bpmTool->togglePlaybackFromShortcut();
                }
                return true;
            }
        }

        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        auto& engine = Logic::EditorEngine::instance();
        bool  playbackShortcutPressed =
            ShortcutUtils::isShortcutPressed(shortcutConfig.togglePlayback);
        // 画笔绘制期间 Shift 是交互修饰键，继续允许它叠加在用户绑定上。
        if ( !playbackShortcutPressed && io.KeyShift &&
             !shortcutConfig.togglePlayback.shift &&
             engine.isActiveSessionDrawingBrush() ) {
            auto shiftedBinding  = shortcutConfig.togglePlayback;
            shiftedBinding.shift = true;
            playbackShortcutPressed =
                ShortcutUtils::isShortcutPressed(shiftedBinding);
        }
        if ( !playbackShortcutPressed ) {
            return false;
        }

        if ( ImGui::IsAnyItemActive() ) {
            const bool timelineMarqueeSelecting =
                context.sourceManager &&
                context.sourceManager->isTimelineTimingMarqueeSelecting();
            const bool timelineTimingDragging =
                context.sourceManager &&
                context.sourceManager->isTimelineTimingDragging();
            const bool allowPlaybackToggle =
                shouldAllowPlaybackToggleWhileItemActive(
                    io.KeyShift,
                    engine.isActiveSessionSelectingMarquee(),
                    engine.isActiveSessionDraggingNote(),
                    engine.isActiveSessionDrawingBrush(),
                    timelineTimingDragging,
                    timelineMarqueeSelecting);
            if ( !allowPlaybackToggle ) return false;
        }

        // 全局播放快捷键已消费按键后，禁止同一按键再激活当前获得导航焦点的
        // 协作跟随按钮或远端位置跳转热区。
        consumePlaybackShortcutNavigationActivation(imguiContext);
        execute(context, MainMenuItemActivation{});
        return true;
    }

private:
    /// @brief 当前帧快捷键显示缓存。
    mutable std::string m_shortcutBuffer;
};
}  // namespace

/// @brief 创建播放暂停切换动作处理器。
std::unique_ptr<IMainMenuItemActionHandler> createTogglePlaybackAction()
{
    return std::make_unique<TogglePlaybackAction>();
}

}  // namespace MMM::UI
