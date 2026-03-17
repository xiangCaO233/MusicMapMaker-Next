#include "canvas/TestCanvas.h"
#include "graphic/imguivk/VKOffScreenRenderer.h"
#include "imgui.h"
#include "log/colorful-log.h"

namespace MMM::Canvas
{
TestCanvas::TestCanvas(uint32_t w, uint32_t h) : Graphic::UI::IRenderableView()
{
    m_targetWidth  = w;
    m_targetHeight = h;
}
TestCanvas::~TestCanvas() {}

// 接口实现
void TestCanvas::update()
{
    // 1. 在 Begin 之前设置窗口大小
    // ImGuiCond_FirstUseEver: 只在第一次运行且没有 .ini 配置文件记录时生效
    // ImGuiCond_Always: 强制每一帧都固定这个大小（用户无法拖拽改变大小）
    // ImGuiCond_Once: 每轮启动只设置一次
    ImGui::SetNextWindowSize(ImVec2((float)m_width, (float)m_height),
                             ImGuiCond_FirstUseEver);

    ImGui::Begin("Music Score");
    // 处理逻辑... 填充 m_brush

    // 1. 获取 ImGui 窗口分配给内容的实际大小
    ImVec2 size = ImGui::GetContentRegionAvail();
    if ( size.x > 0 && size.y > 0 ) {
        // 仅仅是发送请求，不会立刻改变渲染状态
        setTargetSize(static_cast<uint32_t>(size.x),
                      static_cast<uint32_t>(size.y));
    }

    vk::DescriptorSet texID = getDescriptorSet();
    // 增加判空，防止在重构瞬间崩溃
    if ( texID != VK_NULL_HANDLE ) {
        ImGui::Image((ImTextureID)(VkDescriptorSet)texID, size);
    } else {
        ImGui::Text("Loading Surface...");
    }
    ImGui::End();
}

///@brief 是否需要重新记录命令 (比如数据变了)
bool TestCanvas::isDirty() const
{
    return false;
}

}  // namespace MMM::Canvas
