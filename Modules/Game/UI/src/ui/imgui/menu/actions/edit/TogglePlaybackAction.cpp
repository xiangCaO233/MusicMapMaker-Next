#include "config/AppConfig.h"
#include "ui/IEditorApplicationService.h"
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
/// @note 使用稳定窗口 ID 比较，窗口可见标题本地化不会影响焦点识别。
bool isBpmMeasurementToolFocused(const ImGuiContext* context)
{
    // ImGui 上下文或导航窗口缺失时自然得到空起点。
    const ImGuiWindow* window = context ? context->NavWindow : nullptr;
    // 子窗口聚焦时沿父链查找工具根窗口，避免只识别最外层标题。
    while ( window ) {
        // Name 为空时跳过比较，仍继续检查父窗口。
        if ( window->Name &&
             isBpmMeasurementToolStableWindowId(
                 ShortcutUtils::stableWindowId(window->Name)) ) {
            return true;
        }
        // ParentWindow 链由 ImGui 管理，仅在本次输入处理期间观察。
        window = window->ParentWindow;
    }
    return false;
}

/// @brief 播放暂停切换动作。
/// @details 统一动态图标、配置快捷键、BPM 工具空格路由与编辑器播放命令。
class TogglePlaybackAction final : public IMainMenuItemActionHandler
{
public:
    /// @brief 根据当前播放状态返回播放或暂停图标。
    /// @param context 提供编辑器应用服务查询入口。
    /// @param fallbackIcon 默认图标，本动作始终使用播放状态图标。
    /// @return 播放中返回暂停图标，否则返回播放图标。
    /// @warning UI 热路径：只读取服务状态，不得复制共享所有权或访问音频文件。
    const char* icon(const MainMenuContext& context,
                     const char*            fallbackIcon) const override
    {
        (void)fallbackIcon;
        // 服务指针只用于当前菜单帧，所有权仍由 UIManager 管理。
        const auto* service =
            context.sourceManager
                ? context.sourceManager->getEditorApplicationService()
                : nullptr;
        // 服务缺失与停止状态统一呈现播放图标，保持入口可辨识。
        return service && service->isPlaybackPlaying() ? ICON_MMM_PAUSE
                                                       : ICON_MMM_PLAY;
    }

    /// @brief 获取用户配置的播放切换快捷键提示。
    /// @param context 单帧主菜单上下文，本查询无需读取。
    /// @param fallbackShortcut 默认提示；配置为空时本动作选择不显示。
    /// @return 当前配置格式化文本，未绑定时返回 nullptr。
    /// @warning UI 热路径：只格式化内存中的单个快捷键配置。
    const char* shortcut(const MainMenuContext& context,
                         const char*            fallbackShortcut) const override
    {
        (void)context;
        (void)fallbackShortcut;
        // 引用配置避免复制完整快捷键集合。
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        // 成员缓存确保返回的 C 字符串在本帧菜单绘制期间有效。
        m_shortcutBuffer =
            ShortcutUtils::formatShortcut(shortcutConfig.togglePlayback);
        return m_shortcutBuffer.empty() ? nullptr : m_shortcutBuffer.c_str();
    }

    /// @brief 切换播放状态。
    /// @param context 提供编辑器应用服务查询入口。
    /// @param activation 激活来源，不改变切换语义。
    /// @note 通过 CmdSetPlayState 设置目标值，而不是直接操作音频引擎。
    /// @warning 命令分发必须发生在 UI 逻辑路径，保持会话状态同步。
    void execute(MainMenuContext&              context,
                 const MainMenuItemActivation& activation) override
    {
        (void)activation;
        // 服务不可用时没有权威播放状态，动作保持无副作用退出。
        const auto* service =
            context.sourceManager
                ? context.sourceManager->getEditorApplicationService()
                : nullptr;
        if ( !service ) return;
        // 基于执行瞬间状态计算目标，避免菜单绘制与点击之间的状态漂移。
        MenuUtil::dispatchCommand(
            Logic::CmdSetPlayState{ !service->isPlaybackPlaying() });
    }

    /// @brief 消费播放暂停快捷键，并保留 BPM 工具对空格键的专用路由。
    /// @param context 单帧主菜单上下文。
    /// @return 快捷键触发时返回 true。
    /// @warning UI 热路径：每帧只读取输入状态和当前交互状态。
    /// @note BPM 工具对空格的所有权先于用户配置的全局播放绑定判定。
    bool handleShortcut(MainMenuContext& context) override
    {
        // 输入与上下文只在当前 ImGui 帧内有效，不跨帧缓存。
        ImGuiIO&      io           = ImGui::GetIO();
        ImGuiContext* imguiContext = ImGui::GetCurrentContext();
        if ( ImGui::IsKeyPressed(ImGuiKey_Space, false) ) {
            // 仅在空格首次按下时执行 BPM 焦点父链查询。
            const bool bpmToolFocused =
                isBpmMeasurementToolFocused(imguiContext);
            auto* bpmTool =
                bpmToolFocused && context.sourceManager
                    ? context.sourceManager->getView<BpmMeasurementToolView>(
                          "BpmMeasurementTool")
                    : nullptr;
            // 任一修饰键都会改变空格路由策略，避免抢占组合快捷键。
            const bool hasModifier =
                io.KeyCtrl || io.KeyAlt || io.KeySuper || io.KeyShift;
            const BpmSpaceShortcutDisposition bpmDisposition =
                // 纯策略函数集中处理焦点、文本输入和工具可用性组合。
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
                    // 只有明确 ToggleTool 结果才调用工具自身播放控制入口。
                    bpmTool->togglePlaybackFromShortcut();
                }
                // ConsumeOnly 同样返回 true，阻止空格穿透到底层画布。
                return true;
            }
        }

        // BPM 路由未接管后，再解析用户配置的全局播放快捷键。
        const auto& shortcutConfig =
            Config::AppConfig::instance().getEditorSettings().shortcutConfig;
        const auto* service =
            context.sourceManager
                ? context.sourceManager->getEditorApplicationService()
                : nullptr;
        // 无编辑器服务时不能判断交互状态或切换播放。
        if ( !service ) return false;
        // 首先按用户绑定原样检测，不隐式添加修饰键。
        bool playbackShortcutPressed =
            ShortcutUtils::isShortcutPressed(shortcutConfig.togglePlayback);
        // 画笔绘制期间 Shift 是交互修饰键，继续允许它叠加在用户绑定上。
        if ( !playbackShortcutPressed && io.KeyShift &&
             !shortcutConfig.togglePlayback.shift &&
             service->isDrawingBrush() ) {
            // 复制单个轻量绑定并仅修改临时 Shift 位，不污染持久化配置。
            auto shiftedBinding  = shortcutConfig.togglePlayback;
            shiftedBinding.shift = true;
            playbackShortcutPressed =
                ShortcutUtils::isShortcutPressed(shiftedBinding);
        }
        // 未命中时不消费输入，允许其他菜单动作继续判断。
        if ( !playbackShortcutPressed ) {
            return false;
        }

        // 活动 ImGui 项目通常应阻止全局播放切换，编辑手势由策略白名单裁决。
        if ( ImGui::IsAnyItemActive() ) {
            // Timeline 交互状态属于 UIManager，需与编辑器服务状态合并判断。
            const bool timelineMarqueeSelecting =
                context.sourceManager &&
                context.sourceManager->isTimelineTimingMarqueeSelecting();
            const bool timelineTimingDragging =
                context.sourceManager &&
                context.sourceManager->isTimelineTimingDragging();
            const bool allowPlaybackToggle =
                // 策略函数明确列出允许播放切换的连续编辑手势。
                shouldAllowPlaybackToggleWhileItemActive(
                    io.KeyShift,
                    service->isSelectingMarquee(),
                    service->isDraggingNote(),
                    service->isDrawingBrush(),
                    timelineTimingDragging,
                    timelineMarqueeSelecting);
            // 非白名单活动控件保留快捷键所有权，避免意外切歌或停播。
            if ( !allowPlaybackToggle ) return false;
        }

        // 全局播放快捷键已消费按键后，禁止同一按键再激活当前获得导航焦点的
        // 协作跟随按钮或远端位置跳转热区。
        consumePlaybackShortcutNavigationActivation(imguiContext);
        // 复用菜单执行入口，确保快捷键和点击使用相同目标状态命令。
        execute(context, MainMenuItemActivation{});
        return true;
    }

private:
    /// @brief 当前帧快捷键显示缓存。
    /// @note 仅为 shortcut 返回值提供稳定存储，不参与快捷键匹配。
    mutable std::string m_shortcutBuffer;
};
}  // namespace

/// @brief 创建播放暂停切换动作处理器。
/// @return 独占所有权的播放快捷键处理器。
/// @warning 处理器必须在 ImGui 与 UIManager 所属线程使用。
std::unique_ptr<IMainMenuItemActionHandler> createTogglePlaybackAction()
{
    return std::make_unique<TogglePlaybackAction>();
}

}  // namespace MMM::UI
