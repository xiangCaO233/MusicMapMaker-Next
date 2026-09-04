#pragma once

#include "ui/layout/CLayWrapperCore.h"
#include <cstdint>
#include <imgui.h>
#include <string>

namespace MMM::UI
{
class UIManager;
class ICanvasView;
class IAuxiliaryWindowView;
class IParallelUiPreparable;

/// @brief 给当前 ImGui 窗口原生关闭按钮补充统一交互与 Dock
/// 悬浮视觉反馈。
/// @param wasOpenBeforeBegin 调用 ImGui::Begin 前窗口是否处于打开状态。
/// @param pOpen 传给 ImGui::Begin 的打开状态指针。
/// @warning UI 热路径：窗口 Begin 后调用；普通帧只读取 ImGui
/// 状态，Dock 关闭按钮悬浮时只调整宿主已有顶点颜色。
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

    /// @brief 创建视图并分配独立的 Clay 布局上下文。
    IUIView(const std::string& name);

    /// @brief 销毁视图持有的 Clay 布局上下文。
    virtual ~IUIView();

    /// @brief 获取视图具体类型,替代 dynamic_cast
    virtual ViewType getViewType() const { return ViewType::Base; }

    /// @brief 安全转换为 ITextureLoader,替代 dynamic_cast (虚继承下 static_cast
    /// 不可用)
    virtual class ITextureLoader* asTextureLoader() { return nullptr; }

    /// @brief 安全转换为 IRenderableView
    virtual class IRenderableView* asRenderableView() { return nullptr; }

    /// @brief 安全转换为画布能力接口。
    virtual ICanvasView* asCanvasView() { return nullptr; }

    /// @brief 安全转换为独立窗口能力接口。
    virtual IAuxiliaryWindowView* asAuxiliaryWindowView() { return nullptr; }

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
    LayoutContext(CLayWrapperCore::WindowContext& layoutContext,
                  const std::string& windowName, bool customWindowFlags = false,
                  ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar,
                  bool* open = nullptr, ImGuiID dockId = 0,
                  ImGuiCond dockCond = ImGuiCond_Always);

    /// @brief 结束 ImGui 窗口并恢复构造阶段压入的样式。
    ~LayoutContext();

    ImVec2 m_startPos;
    ImVec2 m_avail;
    ImVec2 m_mousePos;
    float  m_dpiScale;
    bool   m_isMouseDown;
};

}  // namespace MMM::UI
