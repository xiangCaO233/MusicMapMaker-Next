#include "ui/imgui/markdown/MarkdownRenderer.h"

#include "imgui.h"

#include <string_view>
#include <utility>

namespace MMM::UI::DesktopPathUtils
{
/// @brief 测试替身；渲染测试不启动外部浏览器。
bool openUrlInBrowser(std::string_view url)
{
    return url.starts_with("https://") || url.starts_with("http://");
}
}  // namespace MMM::UI::DesktopPathUtils

namespace
{
/// @brief 创建一帧 ImGui 内容并覆盖 Markdown 的三个公共绘制入口。
bool testMarkdownLayoutAndRendering()
{
    constexpr std::string_view MARKDOWN =
        "# 标题\n正文包含 **粗体**、`代码` 和 [链接](https://example.com)。\n"
        "- 列表项目\n> 引用内容\n```cpp\nint value = 1;\n```";

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(480.0F, 360.0F));
    ImGui::Begin("MarkdownRendererTest",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove);

    const MMM::UI::MarkdownRenderOptions options{
        .wrapWidth        = 300.0F,
        .interactiveLinks = false,
    };
    const auto measured = MMM::UI::measureMarkdown(MARKDOWN, options);
    MMM::UI::renderMarkdown(MARKDOWN, options);

    auto*                                drawList = ImGui::GetWindowDrawList();
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
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.0F, 0.0F),
                                        ImVec2(300.0F, 96.0F));
    ImGui::BeginTooltip();
    const float maxScrollY = ImGui::GetScrollMaxY();
    if ( scrollToBottom && maxScrollY > 0.0F ) {
        ImGui::SetScrollY(maxScrollY);
    }
    const float scrollY = ImGui::GetScrollY();
    MMM::UI::renderMarkdown(LONG_MARKDOWN);
    ImGui::EndTooltip();
    ImGui::End();
    ImGui::Render();
    return { maxScrollY, scrollY };
}

/// @brief 验证 Tooltip 即使禁止直接输入也保留可编程滚动状态。
bool testLongTooltipCanBeScrolled()
{
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
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io               = ImGui::GetIO();
    io.DisplaySize            = ImVec2(480.0F, 360.0F);
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

    const bool valid =
        testMarkdownLayoutAndRendering() && testLongTooltipCanBeScrolled();
    ImGui::DestroyContext();
    return valid ? 0 : 1;
}
