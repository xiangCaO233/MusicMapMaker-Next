#include "canvas/TestCanvas.h"
#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "imgui.h"

namespace MMM::Canvas
{
TestCanvas::TestCanvas() : Graphic::UI::IRenderableView() {}
TestCanvas::~TestCanvas() {}

// 接口实现
void TestCanvas::update()
{
    ImGui::Begin("Music Score");
    // 处理逻辑... 填充 m_brush

    // 将离屏渲染的结果贴出来
    ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::Image((ImTextureRef)getTextureDescriptor(), size);
    ImGui::End();
}

///@brief 是否需要重新记录命令 (比如数据变了)
bool TestCanvas::isDirty() const
{
    return false;
}

}  // namespace MMM::Canvas
