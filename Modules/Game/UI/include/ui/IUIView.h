#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace MMM::UI
{
class UIManager;
class ICanvasView;
class IAuxiliaryWindowView;
class IParallelUiPreparable;
class LayoutContext;

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
    /// @brief 激活当前视图独占的布局上下文。
    /// @warning UI 热路径：仅切换已创建的上下文，禁止分配和重建 Arena。
    void makeLayoutCurrent();

private:
    friend class LayoutContext;
    /// @brief 隐藏 Clay 类型和 Arena 生命周期，避免基础视图传播布局实现。
    struct LayoutState;
    /// @brief 视图构造时一次分配，整个视图存活期间地址稳定。
    std::unique_ptr<LayoutState> m_layoutState;
};

}  // namespace MMM::UI
