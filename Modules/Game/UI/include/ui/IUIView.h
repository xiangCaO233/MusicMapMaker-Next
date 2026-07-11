#pragma once

#include "config/AppConfig.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"
#include "ui/layout/CLayDefs.h"
#include "ui/layout/CLayWrapperCore.h"
#include <cmath>
#include <imgui.h>
#include <string>

namespace MMM::UI
{
class UIManager;
class IParallelUiPreparable;

/// @brief 给当前 ImGui 窗口原生关闭按钮补充统一交互反馈。
/// @param wasOpenBeforeBegin 调用 ImGui::Begin 前窗口是否处于打开状态。
/// @param pOpen 传给 ImGui::Begin 的打开状态指针。
/// @warning UI 热路径：窗口 Begin 后调用，只读取 ImGui 状态并触发预加载音效。
void FeedbackCurrentWindowCloseButton(bool wasOpenBeforeBegin, bool* pOpen);

/// @brief 视图类型判别枚举,替代 dynamic_cast (支持 -fno-rtti)
enum class ViewType : uint8_t {
    Base,           ///< 基础 IUIView
    TextureLoader,  ///< ITextureLoader + IUIView
    RenderableView  ///< IRenderableView = ITextureLoader + VKOffScreenRenderer
};

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

    /// @brief 获取视图具体类型,替代 dynamic_cast
    virtual ViewType getViewType() const { return ViewType::Base; }

    /// @brief 安全转换为 ITextureLoader,替代 dynamic_cast (虚继承下 static_cast
    /// 不可用)
    virtual class ITextureLoader* asTextureLoader() { return nullptr; }

    /// @brief 安全转换为 IRenderableView
    virtual class IRenderableView* asRenderableView() { return nullptr; }

    /// @brief 安全转换为可并行准备 UI 数据的接口
    virtual IParallelUiPreparable* asParallelUiPreparable() { return nullptr; }

    /// @brief 获取实际实例指针，用于在禁用 RTTI 且存在虚继承时进行下行转换
    virtual void* getActualInstance() { return this; }

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
    /// @brief 创建布局上下文并开始 ImGui 窗口
    LayoutContext(CLayWrapperCore::WindowContext& clayout_ctx,
                  const std::string&              iwindow_name,
                  bool                            custom_window_flags = false,
                  ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar,
                  bool* p_open = nullptr, ImGuiID dockId = 0,
                  ImGuiCond dockCond = ImGuiCond_Always)
    {
        CLayWrapperCore::instance().makeCurrent(clayout_ctx.context);

        // 应用窗口标题字体
        auto&   skinMgr   = Config::SkinManager::instance();
        ImFont* titleFont = skinMgr.getFont("title");
        if ( titleFont ) ImGui::PushFont(titleFont, titleFont->LegacySize);

        // 在 Begin 之前，推入样式变量，将窗口内边距设为 0，并设置圆角
        auto& editorSettings =
            Config::AppConfig::instance().getEditorSettings();
        float dpiScale = Config::AppConfig::instance().getWindowContentScale();
        float windowRound =
            std::floor(editorSettings.aesthetics.windowRounding * dpiScale);
        float frameRound =
            std::floor(editorSettings.aesthetics.frameRounding * dpiScale);
        ImVec2 itemSpacing = {
            std::floor(editorSettings.aesthetics.itemSpacing * dpiScale),
            std::floor(editorSettings.aesthetics.itemSpacing * dpiScale)
        };

        m_dpiScale = dpiScale;
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding,
            ImVec2(
                std::floor(editorSettings.aesthetics.windowPadding * dpiScale),
                std::floor(editorSettings.aesthetics.windowPadding *
                           dpiScale)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, windowRound);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, windowRound);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, frameRound);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, frameRound);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, itemSpacing);

        if ( dockId != 0 ) {
            ImGui::SetNextWindowDockID(dockId, dockCond);
        }

        const bool wasOpenBeforeBegin = p_open != nullptr && *p_open;

        if ( custom_window_flags ) {
            ImGui::Begin(iwindow_name.c_str(), p_open, windowFlags);
        } else {
            ImGui::Begin(iwindow_name.c_str(), p_open);
        }
        FeedbackCurrentWindowCloseButton(wasOpenBeforeBegin, p_open);

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
        ImGui::PopStyleVar(6);
    }

    ImVec2 m_startPos;
    ImVec2 m_avail;
    ImVec2 m_mousePos;
    float  m_dpiScale;
    bool   m_isMouseDown;
};

}  // namespace MMM::UI
