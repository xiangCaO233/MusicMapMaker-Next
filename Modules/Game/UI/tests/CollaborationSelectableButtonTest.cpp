#include "ui/utils/UIWidgetUtils.h"

#include "imgui.h"
#include "imgui_internal.h"

namespace
{
/// @brief 绘制一帧可切换按钮并校验调用前后的颜色栈深度。
/// @param selected 当前选中状态。
/// @return 样式栈配对正确时返回 true。
bool drawSelectorFrame(bool selected)
{
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(200.0F, 100.0F));
    ImGui::Begin("Selector",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove);
    const int stackBefore = GImGui->ColorStack.Size;
    static_cast<void>(MMM::UI::FeedbackSelectableButton(
        "连接房间", selected, ImVec2(120.0F, 0.0F)));
    const bool balanced = GImGui->ColorStack.Size == stackBefore;
    ImGui::End();
    ImGui::Render();
    return balanced;
}
}  // namespace

/// @brief 验证按钮选中状态连续切换时不会破坏 ImGui 样式栈。
/// @return 每种选中状态的样式栈均配对时返回 0。
int main()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io               = ImGui::GetIO();
    io.DisplaySize            = ImVec2(200.0F, 100.0F);
    io.DeltaTime              = 1.0F / 60.0F;
    io.IniFilename            = nullptr;
    unsigned char* fontPixels = nullptr;
    int            fontWidth  = 0;
    int            fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
    if ( !fontPixels || fontWidth <= 0 || fontHeight <= 0 ) {
        ImGui::DestroyContext();
        return 1;
    }
    const bool valid = drawSelectorFrame(false) && drawSelectorFrame(true) &&
                       drawSelectorFrame(false);
    ImGui::DestroyContext();
    return valid ? 0 : 1;
}
