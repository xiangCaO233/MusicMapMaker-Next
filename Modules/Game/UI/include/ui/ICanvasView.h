#pragma once

#include <imgui.h>

namespace MMM::UI
{

/// @brief UI 层访问画布窗口所需的最小能力接口。
/// @warning UI 热路径接口：实现只允许读写画布本地状态，不得执行文件系统访问、
/// 阻塞等待或共享所有权复制。
class ICanvasView
{
public:
    virtual ~ICanvasView() = default;

    /// @brief 获取画布所在 Dock 节点。
    [[nodiscard]] virtual ImGuiID getDockId() const = 0;

    /// @brief 请求下一次 UI 更新时聚焦画布。
    virtual void requestFocus() {}

    /// @brief 请求关闭可关闭的主画布。
    virtual void requestClose() {}

    /// @brief 消费用户取消关闭的状态。
    [[nodiscard]] virtual bool consumeCloseCancelled() { return false; }

    /// @brief 请求把主画布停靠到编辑区中心。
    virtual void requestDockToCenter() {}

    /// @brief 查询时间线上一帧是否持有交互焦点。
    [[nodiscard]] virtual bool wasFocusedLastFrame() const { return false; }

    /// @brief 查询时间线是否正在框选 Timing。
    [[nodiscard]] virtual bool isTimingMarqueeSelecting() const
    {
        return false;
    }

    /// @brief 查询时间线是否正在拖动 Timing。
    [[nodiscard]] virtual bool isTimingDragging() const { return false; }

    /// @brief 查询 Timing 表格是否打开。
    [[nodiscard]] virtual bool isTimingPointsTableOpen() const { return false; }

    /// @brief 设置 Timing 表格打开状态。
    virtual void setTimingPointsTableOpen(bool) {}

    /// @brief 查询批注表格是否打开。
    [[nodiscard]] virtual bool isAnnotationTableOpen() const { return false; }

    /// @brief 设置批注表格打开状态。
    virtual void setAnnotationTableOpen(bool) {}
};

}  // namespace MMM::UI
