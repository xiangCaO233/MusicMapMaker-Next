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
