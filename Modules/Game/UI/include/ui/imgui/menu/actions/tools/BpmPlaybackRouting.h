#pragma once

#include <string_view>

namespace MMM::UI
{

/// @brief BPM 测量工具试听控制的目标路由。
enum class BpmPlaybackRoute {
    /// @brief 未选择有效音轨，不允许执行播放控制。
    Unavailable,

    /// @brief 使用 BPM 工具独立试听通道，不修改活动谱面画布状态。
    Audition,

    /// @brief 选中音轨与活动谱面主音轨一致，和编辑器同步控制。
    SynchronizedWithEditor
};

/// @brief BPM 工具聚焦时对空格快捷键的处理方式。
enum class BpmSpaceShortcutDisposition {
    /// @brief BPM 工具未聚焦，允许后续编辑器快捷键处理。
    NotOwned,

    /// @brief BPM 工具拥有焦点但当前不应切换播放，只消费按键。
    ConsumeOnly,

    /// @brief 仅切换 BPM 工具所选音轨的播放状态。
    ToggleTool
};

/// @brief 根据规范化音频路径键选择 BPM 工具播放路由。
/// @param selectedAudioKey BPM 工具选中音轨的规范化路径键。
/// @param activeMainAudioKey 活动谱面主音轨的规范化路径键。
/// @return 无选择时不可用；路径一致时同步编辑器，否则使用独立试听通道。
constexpr BpmPlaybackRoute resolveBpmPlaybackRoute(
    std::string_view selectedAudioKey, std::string_view activeMainAudioKey)
{
    if ( selectedAudioKey.empty() ) {
        return BpmPlaybackRoute::Unavailable;
    }
    if ( !activeMainAudioKey.empty() &&
         selectedAudioKey == activeMainAudioKey ) {
        return BpmPlaybackRoute::SynchronizedWithEditor;
    }
    return BpmPlaybackRoute::Audition;
}

/// @brief 判断当前路由是否应向活动编辑器派发播放命令。
/// @param route BPM 工具当前播放路由。
/// @return 仅同步编辑器路由返回 true。
constexpr bool shouldDispatchBpmPlaybackToEditor(BpmPlaybackRoute route)
{
    return route == BpmPlaybackRoute::SynchronizedWithEditor;
}

/// @brief 判断 BPM 工具是否可以直接调用独立试听 transport。
/// @param route 当前播放路由。
/// @return 仅 Audition 路由返回 true；同步路由必须派发逻辑命令。
constexpr bool shouldDirectlyControlBpmAudioTransport(BpmPlaybackRoute route)
{
    return route == BpmPlaybackRoute::Audition;
}

/// @brief 判断 BPM 工具聚焦时的空格键是否应切换工具音轨播放状态。
/// @param hasModifier 当前是否按下任意修饰键。
/// @param wantsTextInput 当前焦点控件是否正在接收文字输入。
/// @return 无修饰键且未输入文字时返回 true。
constexpr bool shouldToggleBpmPlaybackFromSpace(bool hasModifier,
                                                bool wantsTextInput)
{
    return !hasModifier && !wantsTextInput;
}

/// @brief 决定空格键由 BPM 工具消费还是继续交给编辑器。
/// @param bpmToolFocused BPM 工具根窗口或子窗口是否拥有焦点。
/// @param hasModifier 当前是否按下任意修饰键。
/// @param wantsTextInput 当前焦点控件是否正在接收文字输入。
/// @param bpmToolAvailable BPM 工具视图实例是否可用。
/// @return 未聚焦时不占用；聚焦时始终消费，输入有效且工具可用时切换工具。
constexpr BpmSpaceShortcutDisposition resolveBpmSpaceShortcutDisposition(
    bool bpmToolFocused, bool hasModifier, bool wantsTextInput,
    bool bpmToolAvailable)
{
    if ( !bpmToolFocused ) {
        return BpmSpaceShortcutDisposition::NotOwned;
    }
    if ( shouldToggleBpmPlaybackFromSpace(hasModifier, wantsTextInput) &&
         bpmToolAvailable ) {
        return BpmSpaceShortcutDisposition::ToggleTool;
    }
    return BpmSpaceShortcutDisposition::ConsumeOnly;
}

/// @brief 判断稳定 ImGui 窗口 ID 是否属于 BPM 测量工具或其子窗口。
/// @param stableWindowId 已移除可见标题前缀的稳定窗口 ID。
/// @return BPM 工具根窗口或子窗口返回 true。
constexpr bool isBpmMeasurementToolStableWindowId(
    std::string_view stableWindowId)
{
    constexpr std::string_view BPM_TOOL_ID = "BpmMeasurementTool";
    return stableWindowId == BPM_TOOL_ID ||
           (stableWindowId.starts_with(BPM_TOOL_ID) &&
            stableWindowId.size() > BPM_TOOL_ID.size() &&
            stableWindowId[BPM_TOOL_ID.size()] == '/');
}

}  // namespace MMM::UI
