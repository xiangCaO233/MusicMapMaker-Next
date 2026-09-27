#include "ui/imgui/markdown/MarkdownParser.h"

#include <array>
#include <cstddef>
#include <string_view>

/// @file MarkdownParserTest.cpp
/// @brief Markdown 安全子集的块级、行内和图片目标解析回归测试。
/// @details 测试只观察访问器产生的零拥有片段顺序，不依赖渲染器或 ImGui。
/// 覆盖标题、段落、列表、引用、分隔线、围栏代码、强调、链接、图片和转义。
///
/// 断言约定：
/// - 围栏开始和结束行不产生 Code 块；
/// - 围栏内部内容不再解释其他 Markdown；
/// - 标题、无序列表和引用移除自身控制标记；
/// - 有序列表保留可见数字前缀；
/// - 分隔线产生空正文；
/// - 普通文本与样式片段保持原始顺序；
/// - 反斜杠转义后的星号属于 Text；
/// - 链接标签与目标分别进入 text 和 destination；
/// - 图片可解析可选标题但不把标题并入目标；
/// - URL 中的嵌套括号保持完整；
/// - 每个访问器产生的片段数量必须精确匹配期望；
/// - 所有结果 string_view 都直接指向测试常量存储。

namespace
{
/// @brief 验证常用 Markdown 块会被稳定拆分并移除控制标记。
/// @return 块类型、正文和数量均与期望一致时返回 true。
bool testBlockParsing()
{
    // 单个文档串联所有受支持的主要块类型，围栏本身不应产生块。
    constexpr std::string_view SOURCE =
        "# 标题\n正文 **重点**\n- 项目\n2. 第二项\n> 引用\n---\n```cpp\nint "
        "value = 1;\n```";
    // 期望顺序与源文档物理行顺序一致。
    constexpr std::array EXPECTED_KINDS{
        MMM::UI::MarkdownBlockKind::Heading,
        MMM::UI::MarkdownBlockKind::Paragraph,
        MMM::UI::MarkdownBlockKind::UnorderedListItem,
        MMM::UI::MarkdownBlockKind::OrderedListItem,
        MMM::UI::MarkdownBlockKind::Quote,
        MMM::UI::MarkdownBlockKind::Separator,
        MMM::UI::MarkdownBlockKind::Code,
    };
    // 块级控制标记被移除，有序列表保留自身编号文本。
    constexpr std::array<std::string_view, EXPECTED_KINDS.size()>
        EXPECTED_TEXTS{
            "标题", "正文 **重点**",  "项目", "2. 第二项", "引用",
            "",     "int value = 1;",
        };

    // 访问器同步推进索引，任何额外、缺失或错位块都使结果失败。
    std::size_t index = 0U;
    bool        valid = true;
    MMM::UI::visitMarkdownBlocks(
        SOURCE, [&](const MMM::UI::MarkdownBlock& block) {
            if ( index >= EXPECTED_KINDS.size() ||
                 block.kind != EXPECTED_KINDS[index] ||
                 block.text != EXPECTED_TEXTS[index] ) {
                valid = false;
            }
            ++index;
        });
    // 最终数量检查捕获解析器提前停止的情况。
    return valid && index == EXPECTED_KINDS.size();
}

/// @brief 验证强调、代码、链接与转义字符的行内解析。
/// @return 片段类型顺序、链接文本、目标和片段总数正确时返回 true。
bool testInlineParsing()
{
    // 普通文本空格会与相邻标记形成独立片段，转义星号必须保持 Text。
    constexpr std::string_view SOURCE =
        "普通 **粗体** *斜体* `代码` [链接](https://example.com) \\*";
    // 期望数组精确记录解析器从左到右的 span 边界。
    constexpr std::array EXPECTED_KINDS{
        MMM::UI::MarkdownInlineKind::Text,
        MMM::UI::MarkdownInlineKind::Strong,
        MMM::UI::MarkdownInlineKind::Text,
        MMM::UI::MarkdownInlineKind::Emphasis,
        MMM::UI::MarkdownInlineKind::Text,
        MMM::UI::MarkdownInlineKind::Code,
        MMM::UI::MarkdownInlineKind::Text,
        MMM::UI::MarkdownInlineKind::Link,
        MMM::UI::MarkdownInlineKind::Text,
        MMM::UI::MarkdownInlineKind::Text,
    };

    std::size_t index = 0U;
    bool        valid = true;
    // 链接除类型外还需单独验证标签与目标视图。
    bool linkChecked = false;
    MMM::UI::visitMarkdownInline(
        SOURCE, [&](const MMM::UI::MarkdownInlineSpan& span) {
            if ( index >= EXPECTED_KINDS.size() ||
                 span.kind != EXPECTED_KINDS[index] ) {
                valid = false;
            }
            if ( span.kind == MMM::UI::MarkdownInlineKind::Link ) {
                // 解析器不执行或改写绝对 HTTP URL。
                linkChecked = span.text == "链接" &&
                              span.destination == "https://example.com";
            }
            ++index;
        });
    return valid && linkChecked && index == EXPECTED_KINDS.size();
}
}  // namespace

/// @brief 运行共享 Markdown 解析回归测试。
/// @return 所有语法断言通过时返回 0。
int main()
{
    /// @brief 覆盖线上 changelog 的相对图片地址、GIF、标题和括号。
    const auto images = []() {
        // 图片与普通链接共享目标解析，但 span 类型必须为 Image。
        unsigned count = 0;
        bool     valid = true;
        MMM::UI::visitMarkdownInline(
            // 第一项含双引号标题，第二项含嵌套括号和单引号标题。
            "![图片](/download/a.png \"标题\") "
            "![动画](https://example.com/a(b).gif '动画标题')",
            [&](const auto& span) {
                if ( span.kind != MMM::UI::MarkdownInlineKind::Image ) return;
                // 可选标题不属于 destination，URL 内嵌括号必须保留。
                valid &= span.destination ==
                         (count == 0U ? "/download/a.png"
                                      : "https://example.com/a(b).gif");
                ++count;
            });
        // 恰好产生两个图片 span，不能把标题拆成额外图片。
        return valid && count == 2U;
    };
    // 三组纯函数场景全部通过才返回成功。
    return testBlockParsing() && testInlineParsing() && images() ? 0 : 1;
}
