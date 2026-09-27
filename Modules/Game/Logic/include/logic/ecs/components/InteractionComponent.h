#pragma once

#include <cstdint>

namespace MMM::Logic
{

/**
 * @brief 交互状态组件
 *
 * 存储 ECS 实体的 UI 交互状态（如悬停、选中、拖拽中等），由逻辑线程基于 UI
 * 线程的指令进行更新。
 */
struct InteractionComponent {
    /// @brief 当前是否为指针悬浮目标。
    bool isHovered{ false };
    /// @brief 当前选择状态，随会话选择操作更新。
    bool isSelected{ false };
    /// @brief 是否正参与拖动，用于临时交互反馈。
    bool isDragging{ false };
    /// @brief 是否带有剪切操作的暂存标记。
    bool isCut{ false };
    /// @brief HoverPart 的底层值，区分物件头、尾或其他命中部位。
    uint8_t hoveredPart{ 0 };
    /// @brief 悬浮子节点序号，-1 表示未定位到子节点。
    int hoveredSubIndex{ -1 };
};

}  // namespace MMM::Logic
