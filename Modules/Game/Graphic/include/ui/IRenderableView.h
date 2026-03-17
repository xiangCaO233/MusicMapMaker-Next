#pragma once

#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "ui/IUIView.h"
#include "ui/brush/Brush.h"

namespace MMM::Graphic
{
namespace UI
{
class Brush;
class IRenderableView : public IUIView, public VKOffScreenRenderer
{
public:
    IRenderableView()                                  = default;
    IRenderableView(IRenderableView&&)                 = delete;
    IRenderableView(const IRenderableView&)            = delete;
    IRenderableView& operator=(IRenderableView&&)      = delete;
    IRenderableView& operator=(const IRenderableView&) = delete;
    ~IRenderableView()                                 = default;

    ///@brief 获取笔刷
    const Brush& getBrush() const { return m_brush; }

    ///@brief 是否可渲染
    bool renderable() override { return true; }

    ///@brief 是否需要重新记录命令 (比如数据变了)
    virtual bool isDirty() const = 0;

protected:
    Brush m_brush;
};
}  // namespace UI
}  // namespace MMM::Graphic
