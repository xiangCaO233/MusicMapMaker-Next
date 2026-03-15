#include "canvas/TestCanvas.h"

namespace MMM::Canvas
{
TestCanvas::TestCanvas() {}
TestCanvas::~TestCanvas() {}

// 接口实现
void TestCanvas::update() {}

MMM::Graphic::VKOffScreenRenderer& TestCanvas::getOffscreenRenderer() {}

const MMM::Graphic::UI::Brush& TestCanvas::getBrush() const {}
}  // namespace MMM::Canvas
