#pragma once

#include <imgui.h>
#include <string>

namespace MMM::UI
{
class IUIView;

/// @brief 单帧窗口和布局区域的栈上作用域，不拥有视图的持久 Clay 状态。
class LayoutContext final
{

public:
    /// @brief 创建布局上下文并开始 ImGui 窗口。
    /// @param view 提供持久上下文的视图，必须比本帧作用域活得更久。
    /// @param windowName 显示标题及稳定窗口 ID，由调用方统一生成。
    /// @note 仅选择已有 Arena，构造和析构不创建或释放持久布局状态。
    LayoutContext(IUIView& view, const std::string& windowName,
                  bool             customWindowFlags = false,
                  ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar,
                  bool* open = nullptr, ImGuiID dockId = 0,
                  ImGuiCond dockCond = ImGuiCond_Always);

    /// @brief 结束 ImGui 窗口并恢复构造阶段压入的样式。
    ~LayoutContext();

    /// @brief 当前窗口内容区域的屏幕起点。
    ImVec2 m_startPos;
    /// @brief 当前窗口可供布局使用的尺寸。
    ImVec2 m_avail;
    /// @brief 当前鼠标屏幕坐标。
    ImVec2 m_mousePos;
    /// @brief 当前帧采用的窗口内容缩放倍率。
    float m_dpiScale;
    /// @brief 当前帧左键按下状态。
    bool m_isMouseDown;
};

}  // namespace MMM::UI
