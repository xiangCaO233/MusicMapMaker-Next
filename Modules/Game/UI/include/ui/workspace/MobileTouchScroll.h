#pragma once
#include <imgui.h>

namespace MMM::UI
{
/// @brief 移动端统一空白拖动滚动，只接管没有控件占用的触摸起点。
/// @details 保存窗口 ID 而非指针，弹层关闭后不会访问已销毁窗口。
class MobileTouchScroll
{
public:
    /// @brief 在业务窗口 Begin 前提交滚动目标，让本帧布局立即使用新位置。
    /// @warning 每帧只解析固定窗口 ID，不访问资源或等待其他线程。
    void beginFrame(float scale = 1.0f);
    /// @brief 控件提交完成后确认按下位置为空白，避免抢走按钮与滑条。
    /// @warning 每帧最多沿当前窗口的父链查找，不遍历全部窗口。
    void endFrame();
    /// @brief 原始画布输入在捕获和最终释放帧都应让出所有权。
    bool ownsPointer() const { return m_captured || m_releaseGuard; }

private:
    /// @brief 释放本组件的 ActiveID，不干预其他控件已经取得的所有权。
    void cancel();
    /// @brief 本次按下的窗口、各轴滚动目标及独立交互身份。
    ImGuiID m_pressedWindow{}, m_xWindow{}, m_yWindow{}, m_activeId{};
    /// @brief 起点和初始滚动位置，后续位移不累计帧差分。
    ImVec2 m_origin{}, m_scrollOrigin{};
    /// @brief 捕获、已跨过抖动区以及释放保护均为 UI 线程私有状态。
    bool m_captured{}, m_dragging{}, m_releaseGuard{};
};
}  // namespace MMM::UI
