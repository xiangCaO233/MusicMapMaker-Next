#pragma once

#include "ui/IUIView.h"

namespace MMM::Canvas
{
class TestCanvas : public Graphic::UI::IUIView
{
public:
    TestCanvas();
    TestCanvas(TestCanvas&&)                 = default;
    TestCanvas(const TestCanvas&)            = default;
    TestCanvas& operator=(TestCanvas&&)      = default;
    TestCanvas& operator=(const TestCanvas&) = default;
    ~TestCanvas();

    void update() override;

private:
};

}  // namespace MMM::Canvas
