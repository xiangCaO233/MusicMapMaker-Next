#include "ui/utils/UIWidgetUtils.h"

#include "imgui.h"
#include "imgui_internal.h"

/// @file CollaborationSelectableButtonTest.cpp
/// @brief 协作房间可选按钮在连续状态切换下的 ImGui 颜色栈平衡测试。
/// @details 测试使用最小无后端 ImGui 上下文逐帧绘制，不触发真实输入、音效或
/// 渲染提交，只比较辅助函数调用前后的内部样式栈深度。

namespace
{
/// @brief 绘制一帧可切换按钮并校验调用前后的颜色栈深度。
/// @param selected 当前选中状态。
/// @return 样式栈配对正确时返回 true。
bool drawSelectorFrame(bool selected)
{
    // 每个状态使用独立帧，覆盖跨帧切换时的 Push/Pop 配对。
    ImGui::NewFrame();
    // 固定窗口几何，避免布局裁剪干扰按钮代码路径。
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(200.0F, 100.0F));
    ImGui::Begin("Selector",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove);
    // 记录调用前深度，按钮是否选中都必须恢复到该值。
    const int stackBefore = GImGui->ColorStack.Size;
    static_cast<void>(MMM::UI::FeedbackSelectableButton(
        "连接房间", selected, ImVec2(120.0F, 0.0F)));
    // 在 End/Render 前读取栈，精确约束辅助函数自身的影响。
    const bool balanced = GImGui->ColorStack.Size == stackBefore;
    ImGui::End();
    ImGui::Render();
    return balanced;
}
}  // namespace

/// @brief 验证按钮选中状态连续切换时不会破坏 ImGui 样式栈。
/// @return 每种选中状态的样式栈均配对时返回 0。
/// @details 先初始化默认字体图集，再按未选中、选中、未选中顺序绘制三帧。
int main()
{
    // 测试直接访问内部颜色栈，因此必须校验版本并创建上下文。
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // 无平台后端模式仍需提供有效显示尺寸、帧间隔和禁用 ini 持久化。
    ImGuiIO& io    = ImGui::GetIO();
    io.DisplaySize = ImVec2(200.0F, 100.0F);
    io.DeltaTime   = 1.0F / 60.0F;
    io.IniFilename = nullptr;
    // 预构建默认字体图集，满足 NewFrame 对字体纹理数据的前置要求。
    unsigned char* fontPixels = nullptr;
    int            fontWidth  = 0;
    int            fontHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
    if ( !fontPixels || fontWidth <= 0 || fontHeight <= 0 ) {
        // 初始化失败时仍销毁上下文，避免测试进程内资源泄漏。
        ImGui::DestroyContext();
        return 1;
    }
    // 往返状态覆盖两种分支以及恢复到初始状态的路径。
    const bool valid = drawSelectorFrame(false) && drawSelectorFrame(true) &&
                       drawSelectorFrame(false);
    // 所有 ImGui 数据读取完成后统一销毁上下文。
    ImGui::DestroyContext();
    return valid ? 0 : 1;
}
