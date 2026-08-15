#include "ui/imgui/markdown/MarkdownParser.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace
{
/// @brief 验证常用 Markdown 块会被稳定拆分并移除控制标记。
bool testBlockParsing()
{
    constexpr std::string_view SOURCE =
        "# 标题\n正文 **重点**\n- 项目\n2. 第二项\n> 引用\n---\n```cpp\nint "
        "value = 1;\n```";
    constexpr std::array EXPECTED_KINDS{
        MMM::UI::MarkdownBlockKind::Heading,
        MMM::UI::MarkdownBlockKind::Paragraph,
        MMM::UI::MarkdownBlockKind::UnorderedListItem,
        MMM::UI::MarkdownBlockKind::OrderedListItem,
        MMM::UI::MarkdownBlockKind::Quote,
        MMM::UI::MarkdownBlockKind::Separator,
        MMM::UI::MarkdownBlockKind::Code,
    };
    constexpr std::array<std::string_view, EXPECTED_KINDS.size()>
        EXPECTED_TEXTS{
            "标题", "正文 **重点**",  "项目", "2. 第二项", "引用",
            "",     "int value = 1;",
        };

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
    return valid && index == EXPECTED_KINDS.size();
}

/// @brief 验证强调、代码、链接与转义字符的行内解析。
bool testInlineParsing()
{
    constexpr std::string_view SOURCE =
        "普通 **粗体** *斜体* `代码` [链接](https://example.com) \\*";
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

    std::size_t index       = 0U;
    bool        valid       = true;
    bool        linkChecked = false;
    MMM::UI::visitMarkdownInline(
        SOURCE, [&](const MMM::UI::MarkdownInlineSpan& span) {
            if ( index >= EXPECTED_KINDS.size() ||
                 span.kind != EXPECTED_KINDS[index] ) {
                valid = false;
            }
            if ( span.kind == MMM::UI::MarkdownInlineKind::Link ) {
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
    return testBlockParsing() && testInlineParsing() ? 0 : 1;
}
