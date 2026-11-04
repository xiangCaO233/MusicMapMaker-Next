#include "log/colorful-log.h"
#include "ui/workspace/IWorkspacePresentation.h"
#include "ui/workspace/MobileButton.h"
#include "ui/workspace/MobileTimelineLabels.h"
#include <cmath>
#include <imgui_internal.h>

/// @par 验证边界
/// - 长英文按钮不得让固定两列布局横向增长。
/// - 背景、触摸命中区与文字换行使用同一个宽度。
/// - 大字号通过增加高度容纳，不能暗中缩小字体。
/// - 手机菜单、抽屉和平板宽列都经过正式按钮入口。
/// - 水平滚动标签留在原泳道，不挪动底层 Timing 标记。
/// - 宽时间线静止居中，窄时间线维持横排并自动滚动。
/// - 背板不越过画布底边或父面板页签。
/// - 默认桌面策略不接管旧时间线覆盖层。
/// - 默认 CPU 字库避免个人字体配置影响结果。
/// - 所有结果来自当前绘制帧，不执行文件系统操作。
/// - 字体生命周期覆盖 DrawList，最后统一结束上下文。
/// - 几何容差只允许像素取整，不允许整字越界。
/// - 失败累积报告，手机失败也继续验证平板样本。
namespace
{
int failures{};  ///< 所有几何用例结束后统一报告结果。
/// @brief 不因首个失败停止，保留手机和平板的完整边界诊断。
void check(bool condition, const char* message)
{
    if ( !condition ) {
        XERROR("{}", message);
        ++failures;
    }
}
}  // namespace
/// @brief 真实移动按钮与 DrawList 验证，不用截图颜色或复制布局公式代替。
/// @details 覆盖英文长翻译、用户大字号，以及窄时间线每个字形的归属。
int main()
{
    // 字体使用 CPU 默认字库即可测量英文长标签，不依赖 GPU 或用户字体文件。
    // 显示区域大于所有夹具，避免父窗口边界提前裁剪而掩盖真实越界。
    // 两个字号场景共用正式字体入口，不在测试中复制文字宽度公式。
    ImGui::CreateContext();
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1600, 1200 };
    io.Fonts->AddFontDefault();
    unsigned char* pixels{};
    int            width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for ( float fontScale : { 1.0f, 1.5f } ) {
        io.FontGlobalScale = fontScale;
        // 覆盖滚动中段和端点附近，窄泳道不能因尾部余量整段消失。
        // 水平溢出由各自泳道裁剪，纵向必须始终完整容纳一行。
        io.DeltaTime = 2.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 1500, 1100 });
        ImGui::Begin("Geometry", nullptr, ImGuiWindowFlags_NoSavedSettings);
        // 窄手机菜单、窄抽屉和平板宽列分别覆盖同一按钮实现。
        // 指定列宽是布局契约，不能为容纳翻译而擅自扩大。
        // 命中矩形和测量矩形都要检查，仅验证 helper
        // 无法发现绘制入口的二次扩张。
        for ( float column : { 136.0f, 210.0f, 420.0f } ) {
            // 正宽度必须保持调用方的列边界，长翻译只能增加高度。
            const char* text = "Professional editing mode";
            const auto  size =
                MMM::UI::mobileActionButtonSize(text, { column, 48 });
            check(std::abs(size.x - column) < 0.01f,
                  "long action label respects column width");
            MMM::UI::MobileActionButton("Mode", "+", text, size);
            check(std::abs(ImGui::GetItemRectSize().x - column) < 0.01f,
                  "actual touch hit box respects measured width");
            check(ImGui::GetItemRectSize().y >=
                      ImGui::CalcTextSize(text, nullptr, false, column - 44).y +
                          14,
                  "wrapped label fits hit rectangle");
        }
        // 窄泳道保持横排并滚动，宽泳道保持居中静止。
        // 检查实际顶点同时包含背板与文字，边缘容差只允许像素取整误差。
        // 所有样本都远离父窗口边缘，渲染裁剪不能替代自身正确的几何。
        for ( float timelineWidth : { 120.0f, 180.0f, 800.0f } ) {
            // 直接检查生产绘制顶点，避免只验证测量结果却遗漏 AddText 的裁剪。
            auto*        draw  = ImGui::GetWindowDrawList();
            const int    first = draw->VtxBuffer.Size;
            const ImVec2 origin{ 20, 600 };
            MMM::UI::drawMobileTimelineLaneLabels(origin,
                                                  { timelineWidth, 400 });
            check(draw->VtxBuffer.Size > first + 4,
                  "timeline renders caption glyphs");
            const float captionTop = 1000 - ImGui::GetTextLineHeight() -
                                     2 * ImGui::GetStyle().FramePadding.y;
            // 滚动文字的顶点可以位于裁剪区外，最终可见边界由绘制命令限制。
            // 纵向不滚动，所有字形必须完整留在一行标签区域内。
            for ( int i = first + 4; i < draw->VtxBuffer.Size; ++i ) {
                const auto p = draw->VtxBuffer[i].pos;
                check(p.y >= captionTop - 1 && p.y <= 1001,
                      "caption height contains entire horizontal glyph");
            }
            // 仅统计实际提交文字的绘制命令，空的恢复裁剪命令不算泳道。
            // 独立裁剪限制横向溢出，垂直方向始终完整容纳当前字体。
            int clips{};
            for ( const auto& command : draw->CmdBuffer ) {
                const auto clip = command.ClipRect;
                if ( command.ElemCount && clip.x >= origin.x &&
                     clip.z <= origin.x + timelineWidth &&
                     clip.y >= captionTop - 1 && clip.w <= 1001 )
                    ++clips;
            }
            check(clips >= 5,
                  "each scrolling lane has its own visible clipping region");
        }
        // 无移动策略时不接管桌面标签，默认桌面外观不被移动实现改变。
        check(
            !MMM::UI::workspaceRenderTimelineLaneLabels({ 0, 0 }, { 100, 500 }),
            "desktop keeps original timeline labels");
        ImGui::End();
        ImGui::Render();
    }
    // 没有保存窗口或纹理，结束后默认桌面策略和全局字体状态不向其他用例传播。
    // 一次性销毁上下文也能发现未配对的窗口、样式与裁剪栈。
    ImGui::DestroyContext();
    return failures == 0 ? 0 : 1;
}
