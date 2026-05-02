#pragma once

#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"
#include "ui/layout/CLayDefs.h"
#include "ui/layout/CLayWrapperCore.h"
#include <imgui.h>
#include <string>

namespace MMM::UI
{
class UIManager;
class IUIView
{
public:
    /// @brief ui名称
    std::string m_name;
    bool        m_isOpen{ true };

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
    virtual void update(UIManager* sourceManager) = 0;

    /// @brief 获取窗口是否打开
    virtual bool isOpen() const { return m_isOpen; }

    /// @brief 设置窗口是否打开
    virtual void setOpen(bool open) { m_isOpen = open; }

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
                  const std::string&              iwindow_name,
                  bool                            custom_window_flags = false,
                  ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar,
                  bool*                           p_open              = nullptr)
    {
        CLayWrapperCore::instance().makeCurrent(clayout_ctx.context);

        // 应用窗口标题字体
        auto&   skinMgr   = Config::SkinManager::instance();
        ImFont* titleFont = skinMgr.getFont("title");
        if ( titleFont ) ImGui::PushFont(titleFont);

        // 在 Begin 之前，推入样式变量，将窗口内边距设为 0
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

        if ( custom_window_flags ) {
            ImGui::Begin(iwindow_name.c_str(), p_open, windowFlags);
        } else {
            ImGui::Begin(iwindow_name.c_str(), p_open);
        }

        // 核心修复：Begin 后立即弹出标题字体，使内容使用默认（content）字体
        if ( titleFont ) ImGui::PopFont();

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

}  // namespace MMM::UI
