#pragma once

namespace MMM::Graphic
{
class VKOffScreenRenderer;
namespace UI
{
class Brush;
class IRenderableView
{
public:
    IRenderableView()                                  = default;
    IRenderableView(IRenderableView&&)                 = default;
    IRenderableView(const IRenderableView&)            = default;
    IRenderableView& operator=(IRenderableView&&)      = default;
    IRenderableView& operator=(const IRenderableView&) = default;
    ~IRenderableView()                                 = default;

    // 获取离屏渲染器接口
    virtual VKOffScreenRenderer& getOffscreenRenderer() = 0;
    virtual const Brush&         getBrush() const       = 0;
};
}  // namespace UI
}  // namespace MMM::Graphic
