#pragma once

#if defined(MMM_ENABLE_X11_FRAME_INTERACTION)

#    include "event/core/EventBus.h"
#    include "event/ui/UpdateDragAreaEvent.h"
#    include "graphic/glfw/window/adapters/IWindowFrameAdapter.h"
#    include "graphic/glfw/window/adapters/IWindowFrameHost.h"
#    include <optional>
#    include <vector>

struct GLFWwindow;

namespace MMM::Graphic
{

/// @brief X11 无原生装饰窗口适配器。
class X11WindowAdapter final : public IWindowFrameAdapter
{
public:
    /// @brief 构造函数。
    /// @param host 主窗口宿主接口。
    explicit X11WindowAdapter(IWindowFrameHost& host);

    /// @brief 默认析构函数。
    ~X11WindowAdapter() override;

    X11WindowAdapter(X11WindowAdapter&&)                 = delete;
    X11WindowAdapter(const X11WindowAdapter&)            = delete;
    X11WindowAdapter& operator=(X11WindowAdapter&&)      = delete;
    X11WindowAdapter& operator=(const X11WindowAdapter&) = delete;

    /// @brief 请求 X11 窗口管理器移动无装饰窗口。
    /// @return 成功交给窗口管理器处理时返回 true。
    bool requestMove() override;

    /// @brief 请求 X11 窗口管理器缩放无装饰窗口。
    /// @param edge 缩放方向。
    /// @return 成功交给窗口管理器处理时返回 true。
    bool requestResize(WindowFrameResizeEdge edge) override;

    /// @brief 判断当前 GLFW 平台是否为 X11。
    /// @return 当前运行在 X11 时返回 true。
    [[nodiscard]] bool supportsClientFrameRequests() const override;

    /// @brief 判断是否需要 UI 层补绘边框和阴影。
    /// @return 当前运行在 X11 时返回 true。
    [[nodiscard]] bool usesClientFrameOverlay() const override;

    /// @brief 刷新 X11 Shape 圆角裁剪。
    void refreshFrameShape() override;

    /// @brief 在 GLFW 鼠标按键回调中处理 X11 无装饰窗口交互。
    /// @param button GLFW 鼠标按键值。
    /// @param action GLFW 鼠标动作值。
    /// @param cursorX 鼠标在窗口客户区中的 X 坐标。
    /// @param cursorY 鼠标在窗口客户区中的 Y 坐标。
    /// @return 已发起 X11 窗口管理器交互时返回 true。
    bool handleClientMouseButton(int button, int action, double cursorX,
                                 double cursorY) override;

    /// @brief 在 GLFW 鼠标移动回调中处理 X11 无装饰窗口交互。
    /// @param cursorX 鼠标在窗口客户区中的 X 坐标。
    /// @param cursorY 鼠标在窗口客户区中的 Y 坐标。
    /// @return 已发起 X11 窗口管理器交互时返回 true。
    bool handleClientCursorPos(double cursorX, double cursorY) override;

    /// @brief 处理主窗口焦点变化，避免重新聚焦时误触发最大化窗口拖动还原。
    /// @param focused 主窗口获得焦点时为 true。
    void handleClientFocusChange(bool focused) override;

private:
    /// @brief 处理拖拽区域更新事件。
    /// @param event 拖拽区域更新事件。
    void onUpdateDragArea(const Event::UpdateDragAreaEvent& event);

    /// @brief 判断鼠标是否命中窗口边缘缩放区域。
    /// @param cursorX 鼠标在窗口客户区中的 X 坐标。
    /// @param cursorY 鼠标在窗口客户区中的 Y 坐标。
    /// @return 命中的缩放方向；未命中时返回 std::nullopt。
    [[nodiscard]] std::optional<WindowFrameResizeEdge> resolveResizeEdge(
        double cursorX, double cursorY) const;

    /// @brief 判断鼠标是否命中标题栏拖拽区域。
    /// @param cursorX 鼠标在窗口客户区中的 X 坐标。
    /// @param cursorY 鼠标在窗口客户区中的 Y 坐标。
    /// @return 命中拖拽区域时返回 true。
    [[nodiscard]] bool isInsideDragArea(double cursorX, double cursorY) const;

    /// @brief 清空当前挂起的 frame 交互状态。
    void resetPendingFrameRequest();

    /// @brief 主窗口宿主接口。
    IWindowFrameHost& m_host;

    /// @brief 关联的 GLFW 主窗口句柄。
    GLFWwindow* m_window{ nullptr };

    /// @brief 拖拽区域更新事件订阅 ID。
    Event::SubscriptionID m_dragAreaSubscription{ 0 };

    /// @brief UI 上报的标题栏拖拽区域缓存。
    std::vector<Event::DragArea> m_dragAreas;

    /// @brief 左键按下后是否等待触发标题栏移动。
    bool m_pendingMove{ false };

    /// @brief 左键按下后是否等待触发窗口缩放。
    bool m_pendingResize{ false };

    /// @brief 重新获得焦点后是否跳过下一次最大化标题栏拖拽。
    bool m_skipNextMaximizedDragPress{ false };

    /// @brief 等待触发的窗口缩放方向。
    WindowFrameResizeEdge m_pendingResizeEdge{ WindowFrameResizeEdge::Right };

    /// @brief 触发移动前记录的按下位置 X。
    double m_pressX{ 0.0 };

    /// @brief 触发移动前记录的按下位置 Y。
    double m_pressY{ 0.0 };
};

}  // namespace MMM::Graphic

#endif
