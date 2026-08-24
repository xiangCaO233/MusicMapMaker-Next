#pragma once

#include "common/EditTool.h"

#include <cstdint>

namespace MMM::Canvas
{

/// @brief 被画布浮层阻挡时需要完成的左键手势类型。
enum class BlockedCanvasLeftGestureEnd : std::uint8_t {
    None,
    Marquee,
    Brush,
    ObjectDrag,
};

/// @brief 指针位于阻挡区域时，本帧需要完成的画布手势。
struct BlockedCanvasGestureCompletion {
    /// @brief 左键释放时需要发送的结束命令。
    BlockedCanvasLeftGestureEnd leftEnd{ BlockedCanvasLeftGestureEnd::None };
    /// @brief 是否已经释放左键并应清理 UI 手势状态。
    bool clearLeftState{ false };
    /// @brief 是否需要结束右键擦除手势。
    bool endErase{ false };
};

/// @brief 判断既有框选拖动是否应连续穿过画布阻挡区。
/// @param tool 当前编辑工具。
/// @param leftDragging 左键是否已经形成拖动手势。
/// @param leftStartedOnCanvas 左键手势是否从画布开始。
/// @param leftStartedOnEntity 左键手势是否从物件开始。
/// @param objectDragStarted 是否已经开始物件拖拽。
/// @return 仅从画布空白处开始的活动框选拖动返回 true。
/// @warning UI 热路径：阻挡区域悬浮时每帧调用，只执行常量布尔判断。
[[nodiscard]] constexpr bool shouldContinueMarqueeAcrossBlockedArea(
    Logic::EditTool tool, bool leftDragging, bool leftStartedOnCanvas,
    bool leftStartedOnEntity, bool objectDragStarted)
{
    return tool == Logic::EditTool::Marquee && leftDragging &&
           leftStartedOnCanvas && !leftStartedOnEntity && !objectDragStarted;
}

/// @brief 解析画布手势在批注区等阻挡区域中的释放行为。
/// @param tool 当前编辑工具。
/// @param leftReleased 本帧是否释放左键。
/// @param rightReleased 本帧是否释放右键。
/// @param leftStartedOnCanvas 左键手势是否从画布开始。
/// @param objectDragStarted 是否已经开始物件拖拽。
/// @param eraseStarted 是否已经开始右键擦除。
/// @return 仅在实际释放按键时请求结束对应手势；按住期间保留原状态。
/// @warning UI 热路径：阻挡区域悬浮时每帧调用，只执行常量布尔判断。
[[nodiscard]] constexpr BlockedCanvasGestureCompletion
resolveBlockedCanvasGestureCompletion(Logic::EditTool tool, bool leftReleased,
                                      bool rightReleased,
                                      bool leftStartedOnCanvas,
                                      bool objectDragStarted, bool eraseStarted)
{
    BlockedCanvasGestureCompletion completion;
    completion.clearLeftState = leftReleased;
    completion.endErase       = rightReleased && eraseStarted;
    if ( !leftReleased ) return completion;

    if ( leftStartedOnCanvas && !objectDragStarted &&
         tool == Logic::EditTool::Marquee ) {
        completion.leftEnd = BlockedCanvasLeftGestureEnd::Marquee;
    } else if ( leftStartedOnCanvas && tool == Logic::EditTool::Draw ) {
        completion.leftEnd = BlockedCanvasLeftGestureEnd::Brush;
    } else if ( objectDragStarted ) {
        completion.leftEnd = BlockedCanvasLeftGestureEnd::ObjectDrag;
    }
    return completion;
}

}  // namespace MMM::Canvas
