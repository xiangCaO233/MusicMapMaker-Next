#pragma once

#include "ui/IRenderableView.h"
#include "ui/IUIView.h"

namespace MMM::Canvas
{
class TestCanvas : public Graphic::UI::IUIView,
                   public Graphic::UI::IRenderableView
{
public:
    TestCanvas();
    TestCanvas(TestCanvas&&)                 = default;
    TestCanvas(const TestCanvas&)            = default;
    TestCanvas& operator=(TestCanvas&&)      = default;
    TestCanvas& operator=(const TestCanvas&) = default;
    ~TestCanvas();

    // 接口实现
    void                               update() override;
    MMM::Graphic::VKOffScreenRenderer& getOffscreenRenderer() override;
    const MMM::Graphic::UI::Brush&     getBrush() const override;

private:
};

}  // namespace MMM::Canvas
