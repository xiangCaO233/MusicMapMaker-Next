#pragma once

#include <string>
#include <string_view>

namespace MMM::Canvas
{

/// @brief 根据会话和房间生命周期归约画布的协作标记。
/// @param wasCollaborationCanvas 画布上一帧是否已绑定协作会话。
/// @param isLogoPlaceholder 当前 Session 是否已重置为欢迎占位页。
/// @param wasLogoPlaceholder 上一帧当前 Session 是否仍为欢迎占位页。
/// @param roomLifecycleActive 当前房间是否处于非 Idle 生命周期。
/// @param wasRoomLifecycleActive 上一帧房间是否处于非 Idle 生命周期。
/// @param isActiveCanvas 当前画布是否属于活动 Session。
/// @return 占位页不标记；真实谱面保留已有标记，并在房间启动或欢迎页被远端
/// 谱面复用时标记活动画布。
/// @warning UI 热路径：每帧只执行常量布尔运算。
[[nodiscard]] constexpr bool resolveCollaborationCanvasState(
    bool wasCollaborationCanvas, bool isLogoPlaceholder,
    bool wasLogoPlaceholder, bool roomLifecycleActive,
    bool wasRoomLifecycleActive, bool isActiveCanvas)
{
    if ( isLogoPlaceholder ) return false;
    return wasCollaborationCanvas ||
           (roomLifecycleActive && isActiveCanvas &&
            (!wasRoomLifecycleActive || wasLogoPlaceholder));
}

/// @brief 构造主画布标签的可见标题。
/// @param fallbackTitle 未加载谱面时使用的回退标题。
/// @param hasBeatmap 当前快照是否包含谱面。
/// @param beatmapName 当前谱面名称。
/// @param isDirty 当前谱面是否存在未保存修改。
/// @param collaborationStatusLabel 协作画布的在线或离线状态标签；普通画布为空。
/// @return 脏谱面以固定前缀 `* ` 开头，协作状态位于谱面名称前的完整标题。
/// @warning UI 热路径：主画布每帧调用，只允许进行一次短字符串构造。
inline std::string makeCanvasTabTitle(
    std::string_view fallbackTitle, bool hasBeatmap,
    std::string_view beatmapName, bool isDirty,
    std::string_view collaborationStatusLabel = {})
{
    std::string title = hasBeatmap && !beatmapName.empty()
                            ? std::string(beatmapName)
                            : std::string(fallbackTitle);
    if ( !collaborationStatusLabel.empty() ) {
        title.insert(0, " ");
        title.insert(0, collaborationStatusLabel);
    }
    if ( hasBeatmap && isDirty ) {
        title.insert(0, "* ");
    }
    return title;
}

}  // namespace MMM::Canvas
