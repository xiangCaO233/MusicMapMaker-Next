#pragma once

#include <imgui.h>

#include <string_view>

namespace MMM::UI
{

/// @brief Markdown 绘制配色。
struct MarkdownStyle {
    /// @brief 普通正文颜色。
    ImU32 textColor{ 0U };
    /// @brief 标题和粗体颜色。
    ImU32 strongColor{ 0U };
    /// @brief 弱强调与引用颜色。
    ImU32 mutedColor{ 0U };
    /// @brief 链接颜色。
    ImU32 linkColor{ 0U };
    /// @brief 行内和围栏代码文字颜色。
    ImU32 codeTextColor{ 0U };
    /// @brief 行内和围栏代码背景颜色。
    ImU32 codeBackgroundColor{ 0U };
    /// @brief 引用竖线和分隔线颜色。
    ImU32 accentColor{ 0U };
};

/// @brief Markdown 绘制选项。
struct MarkdownRenderOptions {
    /// @brief 换行宽度；小于等于零时使用当前 ImGui 可用宽度。
    float wrapWidth{ 0.0F };
    /// @brief 最大绘制高度；小于等于零时不限制。
    float maxHeight{ 0.0F };
    /// @brief DrawList 矩形绘制时跳过的顶部高度，用于滚动长文档。
    float verticalOffset{ 0.0F };
    /// @brief 是否使用适合卡片预览的紧凑间距。
    bool compact{ false };
    /// @brief 是否允许点击 HTTP(S) 链接。
    bool interactiveLinks{ true };
    /// @brief 自定义配色；为空时使用当前 ImGui 主题生成。
    const MarkdownStyle* style{ nullptr };
};

/// @brief Markdown 排版结果。
struct MarkdownLayoutResult {
    /// @brief 实际使用的宽高。
    ImVec2 size{ 0.0F, 0.0F };
    /// @brief 内容是否因高度限制而被裁剪。
    bool truncated{ false };
    /// @brief 当前鼠标是否悬浮于可交互链接。
    bool linkHovered{ false };
};

/// @brief 生成基于当前 ImGui 主题的 Markdown 配色。
/// @return 可供当前帧使用的配色值。
MarkdownStyle defaultMarkdownStyle();

/// @brief 测量 Markdown 文档的布局尺寸。
/// @param markdown Markdown 源文本。
/// @param options 绘制选项。
/// @return 使用相同渲染规则得到的尺寸与裁剪状态。
/// @warning UI 可见路径：线性解析输入并计算字形，不执行外部操作。
MarkdownLayoutResult measureMarkdown(
    std::string_view             markdown,
    const MarkdownRenderOptions& options = MarkdownRenderOptions{});

/// @brief 在当前 ImGui 布局位置绘制 Markdown 文档并推进光标。
/// @param markdown Markdown 源文本。
/// @param options 绘制选项。
/// @warning UI 可见路径：仅在内容可见时调用；链接只接受 HTTP(S) 协议。
void renderMarkdown(
    std::string_view             markdown,
    const MarkdownRenderOptions& options = MarkdownRenderOptions{});

/// @brief 在指定矩形中使用 ImDrawList 绘制 Markdown 文档。
/// @param drawList 目标绘制列表。
/// @param min 矩形左上角屏幕坐标。
/// @param max 矩形右下角屏幕坐标。
/// @param markdown Markdown 源文本。
/// @param options 绘制选项。
/// @return 实际使用的尺寸与裁剪状态。
/// @warning UI 热路径：批注卡片可见时调用；线性解析文本且不分配文档树。
MarkdownLayoutResult renderMarkdownToDrawList(
    ImDrawList& drawList, const ImVec2& min, const ImVec2& max,
    std::string_view             markdown,
    const MarkdownRenderOptions& options = MarkdownRenderOptions{});

}  // namespace MMM::UI
