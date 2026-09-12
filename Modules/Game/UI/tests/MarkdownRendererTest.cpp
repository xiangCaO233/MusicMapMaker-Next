#include "ui/imgui/markdown/MarkdownRenderer.h"

#include "imgui.h"

#include <string_view>
#include <utility>

/// @file MarkdownRendererTest.cpp
/// @brief Markdown 测量、绘制、裁剪、滚动与图片布局的无 GPU 回归测试。
/// @details 测试使用纯 ImGui DrawList 验证公共入口，不创建平台窗口或渲染后端；
/// 外部浏览器函数由本翻译单元提供替身，确保测试不会产生系统副作用。
///
/// 覆盖场景：
/// - 测量与当前窗口绘制返回正尺寸；
/// - 标题、强调、代码、链接、列表、引用和代码块可共同排版；
/// - 显式矩形高度限制会设置 truncated；
/// - verticalOffset 允许外部滚动查看被裁内容；
/// - 长 Tooltip 产生并保持可编程纵向滚动位置；
/// - 图片查询结果生成纹理 draw command；
/// - 图片按可用宽度保持宽高比缩放；
/// - 图片替代文字始终占据布局空间；
/// - 长替代文字在窄区域自动换行。
///
/// 断言边界：
/// - 测量结果宽度不超过显式 wrapWidth；
/// - 普通文档高度大于单行字体高度；
/// - 无高度限制的测量不应截断；
/// - 受限绘制高度不超过 maxHeight；
/// - 带 verticalOffset 的结果保留完整滚动坐标；
/// - Tooltip 在内容建立后的后续帧具有正滚动上限；
/// - 程序设置滚动位置后下一帧仍为正；
/// - 图片命令使用缓存提供的纹理 ID；
/// - 2:1 图片缩放后仍保持 2:1；
/// - 图片标签增加布局高度；
/// - 长标签相对短标签进一步增加高度；
/// - 所有测试窗口均在同帧正确 Begin、End 和 Render。

namespace MMM::UI::DesktopPathUtils
{
/// @brief 测试替身；渲染测试不启动外部浏览器。
/// @param url 渲染器请求打开的目标。
/// @return 仅 HTTP(S) 目标返回 true。
bool openUrlInBrowser(std::string_view url)
{
    return url.starts_with("https://") || url.starts_with("http://");
}
}  // namespace MMM::UI::DesktopPathUtils

namespace
{
/// @brief 测试用缓存不创建 GPU 资源，只返回稳定纹理号与帧区域。
class TestImages final : public MMM::UI::IMarkdownImages
{
public:
    /// @brief 返回 2:1 图片和图集中第一帧的 UV。
    /// @return 固定伪纹理 ID、640x320 尺寸和左上四分之一区域。
    MMM::UI::MarkdownImage findImage(std::string_view) const override
    {
        return {
            ImTextureID{ 42 }, { 640, 320 }, { 0, 0 }, { 0.5F, 0.5F }, false
        };
    }
};
/// @brief 验证图片纹理、宽度缩放与常驻标签排版，长标签必须自动换行。
/// @return 纹理命令、宽度、标签高度和换行高度均正确时返回 true。
bool testEmbeddedImage()
{
    // 图片服务返回稳定伪纹理，不需要真实 GPU 资源。
    TestImages images;
    ImGui::NewFrame();
    ImGui::Begin("EmbeddedImageTest");
    // 200 像素宽度迫使 640x320 图片缩放为 200x100。
    const MMM::UI::MarkdownRenderOptions options{ .wrapWidth = 200.0F,
                                                  .images    = &images };
    // 分别测量有标签、无标签和需多行换行的长标签。
    const auto measured =
        MMM::UI::measureMarkdown("![图片](/image.gif \"标题\")", options);
    const auto unlabelled =
        MMM::UI::measureMarkdown("![](/image.gif)", options);
    const auto longLabel = MMM::UI::measureMarkdown(
        "![This image label is long enough to wrap across several lines in a "
        "narrow reading area](/image.gif)",
        options);
    // 实际绘制应使用同一布局并提交伪纹理 ID。
    MMM::UI::renderMarkdown("![图片](/image.gif \"标题\")", options);
    bool textured = false;
    for ( const auto& command : ImGui::GetWindowDrawList()->CmdBuffer ) {
        // 图片矩形至少生成两个三角形，即六个索引元素。
        textured |=
            command.GetTexID() == ImTextureID{ 42 } && command.ElemCount >= 6;
    }
    ImGui::End();
    ImGui::Render();
    // 标签比无标签图更高，长标签又应比短标签占用更多高度。
    return textured && measured.size.x == 200.0F && measured.size.y >= 100.0F &&
           measured.size.y > unlabelled.size.y &&
           longLabel.size.y > measured.size.y;
}
/// @brief 创建一帧 ImGui 内容并覆盖 Markdown 的三个公共绘制入口。
/// @return 普通测量未截断、两个受限绘制均正确截断时返回 true。
bool testMarkdownLayoutAndRendering()
{
    // 单个文档组合所有渲染器支持的主要块和行内样式。
    constexpr std::string_view MARKDOWN =
        "# 标题\n正文包含 **粗体**、`代码` 和 [链接](https://example.com)。\n"
        "- 列表项目\n> 引用内容\n```cpp\nint value = 1;\n```";

    // 固定无标题窗口提供稳定内容区和 DrawList。
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(480.0F, 360.0F));
    ImGui::Begin("MarkdownRendererTest",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove);

    // 普通入口使用 300 像素换行宽度且禁用外部链接交互。
    const MMM::UI::MarkdownRenderOptions options{
        .wrapWidth        = 300.0F,
        .interactiveLinks = false,
    };
    // 先测量再在当前光标绘制，验证两个入口共享布局逻辑。
    const auto measured = MMM::UI::measureMarkdown(MARKDOWN, options);
    MMM::UI::renderMarkdown(MARKDOWN, options);

    auto* drawList = ImGui::GetWindowDrawList();
    // 24 像素高度不足以容纳全文，必须报告截断。
    const MMM::UI::MarkdownRenderOptions clippedOptions{
        .wrapWidth        = 180.0F,
        .maxHeight        = 24.0F,
        .compact          = true,
        .interactiveLinks = false,
    };
    const auto clipped =
        MMM::UI::renderMarkdownToDrawList(*drawList,
                                          ImVec2(280.0F, 20.0F),
                                          ImVec2(460.0F, 44.0F),
                                          MARKDOWN,
                                          clippedOptions);
    // 第二矩形上移排版原点，模拟外部滚动 36 像素。
    const MMM::UI::MarkdownRenderOptions scrolledOptions{
        .wrapWidth        = 180.0F,
        .maxHeight        = 24.0F,
        .verticalOffset   = 36.0F,
        .compact          = true,
        .interactiveLinks = false,
    };
    const auto scrolled =
        MMM::UI::renderMarkdownToDrawList(*drawList,
                                          ImVec2(280.0F, 52.0F),
                                          ImVec2(460.0F, 76.0F),
                                          MARKDOWN,
                                          scrolledOptions);

    ImGui::End();
    ImGui::Render();
    // 滚动结果高度包含 verticalOffset，仍需报告文档超出裁剪区。
    return measured.size.x > 0.0F && measured.size.y > ImGui::GetFontSize() &&
           !measured.truncated && clipped.truncated &&
           clipped.size.y <= clippedOptions.maxHeight && scrolled.truncated &&
           scrolled.size.y > scrolledOptions.verticalOffset;
}

/// @brief 绘制一帧受高度约束的长 Markdown Tooltip。
/// @param scrollToBottom 本帧是否请求滚动到底部。
/// @return Tooltip 进入本帧时的最大滚动范围与当前位置。
std::pair<float, float> renderLongTooltipFrame(bool scrollToBottom)
{
    // 多个长列表项保证 96 像素 Tooltip 高度产生滚动范围。
    constexpr std::string_view LONG_MARKDOWN =
        "# Changelog\n"
        "- 第一项很长的更新说明，用于验证 Tooltip 会产生纵向滚动范围。\n"
        "- 第二项很长的更新说明，用于验证 Tooltip 会产生纵向滚动范围。\n"
        "- 第三项很长的更新说明，用于验证 Tooltip 会产生纵向滚动范围。\n"
        "- 第四项很长的更新说明，用于验证 Tooltip 会产生纵向滚动范围。\n"
        "- 第五项很长的更新说明，用于验证 Tooltip 会产生纵向滚动范围。";

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(480.0F, 360.0F));
    ImGui::Begin("MarkdownTooltipScrollTest",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove);
    // Tooltip 固定宽度并限制最大高度，启用内部滚动状态。
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.0F, 0.0F),
                                        ImVec2(300.0F, 96.0F));
    ImGui::BeginTooltip();
    // 在绘制本帧内容前读取上一帧计算出的滚动范围。
    const float maxScrollY = ImGui::GetScrollMaxY();
    if ( scrollToBottom && maxScrollY > 0.0F ) {
        // 第二帧显式请求滚到底部，第三帧验证状态保留。
        ImGui::SetScrollY(maxScrollY);
    }
    const float scrollY = ImGui::GetScrollY();
    MMM::UI::renderMarkdown(LONG_MARKDOWN);
    ImGui::EndTooltip();
    ImGui::End();
    ImGui::Render();
    // 返回进入本帧时的范围和位置供跨帧断言。
    return { maxScrollY, scrollY };
}

/// @brief 验证 Tooltip 即使禁止直接输入也保留可编程滚动状态。
bool testLongTooltipCanBeScrolled()
{
    // 首帧建立内容，第二帧设置滚动，第三帧观察持久状态。
    renderLongTooltipFrame(false);
    const auto secondFrame = renderLongTooltipFrame(true);
    const auto thirdFrame  = renderLongTooltipFrame(false);
    return secondFrame.first > 0.0F && thirdFrame.first > 0.0F &&
           thirdFrame.second > 0.0F;
}
}  // namespace

/// @brief 运行共享 Markdown 绘制回归测试。
/// @return 所有布局和 ImGui 状态断言通过时返回 0。
int main()
{
    // 创建无后端 ImGui 上下文并固定显示尺寸和 60 FPS 帧时间。
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io    = ImGui::GetIO();
    io.DisplaySize = ImVec2(480.0F, 360.0F);
    io.DeltaTime   = 1.0F / 60.0F;
    // 禁用 ini 文件，避免读取或写入用户窗口布局。
    io.IniFilename            = nullptr;
    unsigned char* fontPixels = nullptr;
    int            fontWidth  = 0;
    int            fontHeight = 0;
    // 构建字体图集后文本测量和 DrawList 生成才有稳定字体数据。
    io.Fonts->GetTexDataAsRGBA32(&fontPixels, &fontWidth, &fontHeight);
    if ( !fontPixels || fontWidth <= 0 || fontHeight <= 0 ) {
        // 字体初始化失败时先销毁上下文再返回。
        ImGui::DestroyContext();
        return 1;
    }

    // 所有公共布局、滚动和图片场景必须同时通过。
    const bool valid = testMarkdownLayoutAndRendering() &&
                       testLongTooltipCanBeScrolled() && testEmbeddedImage();
    // 清理全局 ImGui 状态，测试进程不保留窗口或字体资源。
    ImGui::DestroyContext();
    return valid ? 0 : 1;
}
