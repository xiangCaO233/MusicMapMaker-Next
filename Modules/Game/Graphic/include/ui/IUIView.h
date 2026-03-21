#pragma once

#include "ui/layout/CLayDefs.h"
#include "ui/layout/CLayWrapperCore.h"
#include <imgui.h>
#include <string>

namespace MMM::Graphic::UI
{
class IUIView
{
public:
    /// @brief ui名称
    const std::string m_name;

    IUIView(const std::string& name) : m_name(name)
    {
        // 创建独立的布局上下文
        m_layoutCtx = CLayWrapperCore::instance().createWindowContext();
    }
    virtual ~IUIView()
    {
        CLayWrapperCore::instance().destroyWindowContext(m_layoutCtx);
    }

    /// @brief 更新ui
    virtual void update() = 0;

    /// @brief 是否可渲染
    /// @return 默认不可再渲染
    virtual bool renderable() { return false; }

protected:
    /// 布局上下文
    CLayWrapperCore::WindowContext m_layoutCtx;
};

class LayoutContext final
{

public:
    LayoutContext(CLayWrapperCore::WindowContext& clayout_ctx,
                  const char*                     iwindow_name)
    {
        CLayWrapperCore::instance().makeCurrent(clayout_ctx.context);
        // 在 Begin 之前，推入样式变量，将窗口内边距设为 0
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

        ImGui::Begin(iwindow_name);
        // 1. 获取 ImGui 的绘图起始点（绝对坐标）
        m_startPos = ImGui::GetCursorScreenPos();
        m_avail    = ImGui::GetContentRegionAvail();
        // 1. 获取鼠标状态并传给 Clay
        m_mousePos    = ImGui::GetMousePos();
        m_isMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

        // 告诉Clay相对于布局起点坐标
        Clay_SetPointerState(
            { m_mousePos.x - m_startPos.x, m_mousePos.y - m_startPos.y },
            m_isMouseDown);
    }
    ~LayoutContext()
    {
        // 4. 重置 ImGui 游标，防止 Dummy 影响后续内容
        ImGui::SetCursorScreenPos(m_startPos);
        ImGui::Dummy(m_avail);  // 占位，确保滚动条正确

        ImGui::End();
        // 恢复样式，否则会影响到后面其他的窗口
        ImGui::PopStyleVar();
    }

    ImVec2 m_startPos;
    ImVec2 m_avail;
    ImVec2 m_mousePos;
    bool   m_isMouseDown;
};

}  // namespace MMM::Graphic::UI
