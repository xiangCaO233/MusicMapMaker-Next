#pragma once

#if defined(__APPLE__)

#    include "event/core/EventBus.h"
#    include "event/ui/UpdateDragAreaEvent.h"
#    include "graphic/glfw/window/adapters/IWindowFrameAdapter.h"
#    include "graphic/glfw/window/adapters/IWindowFrameHost.h"
#    include <optional>
#    include <vector>

struct GLFWwindow;

namespace MMM::Graphic
{

/// @brief macOS 无原生装饰窗口适配器。
class MacOSWindowAdapter final : public IWindowFrameAdapter
{
public:
    /// @brief 构造函数。
    /// @param host 主窗口宿主接口。
    explicit MacOSWindowAdapter(IWindowFrameHost& host);

    /// @brief 默认析构函数。
    ~MacOSWindowAdapter() override;

    MacOSWindowAdapter(MacOSWindowAdapter&&)                 = delete;
    MacOSWindowAdapter(const MacOSWindowAdapter&)            = delete;
    MacOSWindowAdapter& operator=(MacOSWindowAdapter&&)      = delete;
    MacOSWindowAdapter& operator=(const MacOSWindowAdapter&) = delete;

    /// @brief 使用原始左键事件请求 AppKit 开始原生窗口拖动。
    /// @return 成功将拖动交给 Window Server 时返回 true。
    bool requestMove() override;

    /// @brief 开始由客户端驱动的 macOS 无边框窗口缩放。
    /// @param edge 缩放方向。
    /// @return 成功记录缩放起始状态时返回 true。
    bool requestResize(WindowFrameResizeEdge edge) override;

    /// @brief 判断是否需要 UI 层显示无边框窗口缩放光标。
    /// @return 主窗口句柄有效时返回 true。
    [[nodiscard]] bool supportsClientFrameRequests() const override;

    /// @brief 判断是否需要 UI 层补绘边框和阴影。
    /// @return macOS 固定返回 false。
    [[nodiscard]] bool usesClientFrameOverlay() const override;

    /// @brief 刷新 macOS 普通窗口的内容圆角和原生阴影。
    void refreshFrameShape() override;

    /// @brief 在 GLFW 鼠标按键回调中记录 macOS 标题栏拖动候选。
    /// @param button GLFW 鼠标按键值。
    /// @param action GLFW 鼠标动作值。
    /// @param cursorX 鼠标在窗口客户区中的 X 坐标。
    /// @param cursorY 鼠标在窗口客户区中的 Y 坐标。
    /// @return 已发起 AppKit 窗口拖动时返回 true。
    /// @warning 输入热路径：GLFW 鼠标回调中执行；只做常量规模命中判断。
    bool handleClientMouseButton(int button, int action, double cursorX,
                                 double cursorY) override;

    /// @brief 在 GLFW 鼠标移动回调中处理 macOS 最大化拖动还原。
    /// @param cursorX 鼠标在窗口客户区中的 X 坐标。
    /// @param cursorY 鼠标在窗口客户区中的 Y 坐标。
    /// @return 已发起 AppKit 窗口拖动时返回 true。
    /// @warning 输入热路径：GLFW 鼠标移动回调中执行；只做常量规模命中判断。
    bool handleClientCursorPos(double cursorX, double cursorY) override;

    /// @brief 处理主窗口焦点变化，避免重新聚焦时沿用旧拖动候选状态。
    /// @param focused 主窗口获得焦点时为 true。
    void handleClientFocusChange(bool focused) override;

private:
    /// @brief 处理拖拽区域更新事件。
    /// @param event 拖拽区域更新事件。
    void onUpdateDragArea(const Event::UpdateDragAreaEvent& event);

    /// @brief 安装 Cocoa content view 的原生拖动命中桥接。
    void installNativeDragBridge();

    /// @brief 移除 Cocoa content view 上关联的原生拖动命中桥接。
    void removeNativeDragBridge();

    /// @brief 判断鼠标是否命中窗口边缘缩放区域。
    /// @param cursorX 鼠标在窗口客户区中的 X 坐标。
    /// @param cursorY 鼠标在窗口客户区中的 Y 坐标。
    /// @return 命中的缩放方向；未命中时返回 std::nullopt。
    [[nodiscard]] std::optional<WindowFrameResizeEdge> resolveResizeEdge(
        double cursorX, double cursorY) const;

    /// @brief 使用当前屏幕鼠标位置更新活动缩放矩形。
    /// @return 成功向 AppKit 提交窗口矩形时返回 true。
    /// @warning 输入热路径：仅在左键边缘缩放期间执行一次常量规模几何计算和
    /// AppKit 窗口矩形更新。
    bool updateActiveResize();

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

    /// @brief UI 上报的标题栏拖拽排除区域缓存。
    std::vector<Event::DragArea> m_blockedDragAreas;

    /// @brief 左键按下后是否等待触发标题栏移动。
    bool m_pendingMove{ false };

    /// @brief 当前是否正在执行客户端驱动的边缘缩放。
    bool m_resizeActive{ false };

    /// @brief 当前活动缩放方向。
    WindowFrameResizeEdge m_resizeEdge{ WindowFrameResizeEdge::Right };

    /// @brief 触发移动前记录的按下位置 X。
    double m_pressX{ 0.0 };

    /// @brief 触发移动前记录的按下位置 Y。
    double m_pressY{ 0.0 };

    /// @brief 缩放开始时鼠标的屏幕 X 坐标。
    double m_resizeStartMouseX{ 0.0 };

    /// @brief 缩放开始时鼠标的屏幕 Y 坐标。
    double m_resizeStartMouseY{ 0.0 };

    /// @brief 缩放开始时窗口左下角的屏幕 X 坐标。
    double m_resizeStartFrameX{ 0.0 };

    /// @brief 缩放开始时窗口左下角的屏幕 Y 坐标。
    double m_resizeStartFrameY{ 0.0 };

    /// @brief 缩放开始时窗口宽度。
    double m_resizeStartFrameWidth{ 0.0 };

    /// @brief 缩放开始时窗口高度。
    double m_resizeStartFrameHeight{ 0.0 };
};

}  // namespace MMM::Graphic

#endif
