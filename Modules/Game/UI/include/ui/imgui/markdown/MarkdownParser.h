#pragma once

#include <cctype>
#include <cstddef>
#include <string_view>

namespace MMM::UI
{

/// @brief Markdown 块类型。
enum class MarkdownBlockKind {
    Paragraph,
    Heading,
    UnorderedListItem,
    OrderedListItem,
    Quote,
    Code,
    Separator,
    Blank,
};

/// @brief 单个 Markdown 块的零拥有解析结果。
struct MarkdownBlock {
    /// @brief 块类型。
    MarkdownBlockKind kind{ MarkdownBlockKind::Paragraph };
    /// @brief 已移除块级标记的正文。
    std::string_view text;
    /// @brief 标题级别或列表缩进层级。
    std::size_t level{ 0U };
};

/// @brief Markdown 行内样式类型。
enum class MarkdownInlineKind {
    Text,
    Strong,
    Emphasis,
    Code,
    Link,
};

/// @brief 单个 Markdown 行内片段的零拥有解析结果。
struct MarkdownInlineSpan {
    /// @brief 片段样式。
    MarkdownInlineKind kind{ MarkdownInlineKind::Text };
    /// @brief 展示文本。
    std::string_view text;
    /// @brief 链接目标；仅 Link 类型有效。
    std::string_view destination;
};

namespace MarkdownParserDetail
{
/// @brief 移除字符串左侧的空格和制表符。
inline std::string_view trimLeft(std::string_view text)
{
    while ( !text.empty() && (text.front() == ' ' || text.front() == '\t') ) {
        text.remove_prefix(1U);
    }
    return text;
}

/// @brief 判断一行是否为 Markdown 分隔线。
inline bool isSeparator(std::string_view line)
{
    line               = trimLeft(line);
    char        marker = '\0';
    std::size_t count  = 0U;
    for ( const char value : line ) {
        if ( value == ' ' || value == '\t' ) continue;
        if ( marker == '\0' ) marker = value;
        if ( value != marker ||
             (value != '-' && value != '*' && value != '_') ) {
            return false;
        }
        ++count;
    }
    return count >= 3U;
}

/// @brief 查找下一个未转义的指定字符。
inline std::size_t findUnescaped(std::string_view text, char needle,
                                 std::size_t offset)
{
    while ( offset < text.size() ) {
        const auto found = text.find(needle, offset);
        if ( found == std::string_view::npos ) return found;
        std::size_t slashCount = 0U;
        for ( std::size_t index = found; index > 0U && text[index - 1U] == '\\';
              --index ) {
            ++slashCount;
        }
        if ( slashCount % 2U == 0U ) return found;
        offset = found + 1U;
    }
    return std::string_view::npos;
}
}  // namespace MarkdownParserDetail

/// @brief 逐块访问 Markdown 文档，不分配内存。
/// @param markdown Markdown 源文本。
/// @param visitor 接收 MarkdownBlock 的回调。
/// @warning UI 可见路径：线性扫描输入文本，不执行 HTML、脚本、网络或文件操作。
template<typename Visitor>
void visitMarkdownBlocks(std::string_view markdown, Visitor&& visitor)
{
    bool        codeBlock = false;
    std::size_t offset    = 0U;
    while ( offset <= markdown.size() ) {
        const auto lineEnd = markdown.find('\n', offset);
        auto       line    = markdown.substr(offset,
                                             lineEnd == std::string_view::npos
                                                 ? std::string_view::npos
                                                 : lineEnd - offset);
        if ( line.ends_with('\r') ) line.remove_suffix(1U);

        const auto trimmed = MarkdownParserDetail::trimLeft(line);
        if ( trimmed.starts_with("```") || trimmed.starts_with("~~~") ) {
            codeBlock = !codeBlock;
        } else if ( codeBlock ) {
            visitor(MarkdownBlock{ MarkdownBlockKind::Code, line, 0U });
        } else if ( trimmed.empty() ) {
            visitor(MarkdownBlock{ MarkdownBlockKind::Blank, {}, 0U });
        } else if ( MarkdownParserDetail::isSeparator(trimmed) ) {
            visitor(MarkdownBlock{ MarkdownBlockKind::Separator, {}, 0U });
        } else {
            std::size_t headingLevel = 0U;
            while ( headingLevel < trimmed.size() && headingLevel < 6U &&
                    trimmed[headingLevel] == '#' ) {
                ++headingLevel;
            }
            if ( headingLevel > 0U && headingLevel < trimmed.size() &&
                 trimmed[headingLevel] == ' ' ) {
                visitor(MarkdownBlock{
                    MarkdownBlockKind::Heading,
                    trimmed.substr(headingLevel + 1U),
                    headingLevel,
                });
            } else if ( trimmed.starts_with(">") ) {
                auto content = trimmed.substr(1U);
                if ( content.starts_with(' ') ) content.remove_prefix(1U);
                visitor(MarkdownBlock{
                    MarkdownBlockKind::Quote,
                    content,
                    0U,
                });
            } else if ( trimmed.size() > 2U && (trimmed.starts_with("- ") ||
                                                trimmed.starts_with("* ") ||
                                                trimmed.starts_with("+ ")) ) {
                const std::size_t indent =
                    static_cast<std::size_t>(trimmed.data() - line.data()) / 2U;
                visitor(MarkdownBlock{
                    MarkdownBlockKind::UnorderedListItem,
                    trimmed.substr(2U),
                    indent,
                });
            } else {
                std::size_t numberEnd = 0U;
                while ( numberEnd < trimmed.size() &&
                        std::isdigit(
                            static_cast<unsigned char>(trimmed[numberEnd])) ) {
                    ++numberEnd;
                }
                if ( numberEnd > 0U && numberEnd + 1U < trimmed.size() &&
                     (trimmed[numberEnd] == '.' || trimmed[numberEnd] == ')') &&
                     trimmed[numberEnd + 1U] == ' ' ) {
                    const std::size_t indent =
                        static_cast<std::size_t>(trimmed.data() - line.data()) /
                        2U;
                    visitor(MarkdownBlock{
                        MarkdownBlockKind::OrderedListItem,
                        trimmed,
                        indent,
                    });
                } else {
                    visitor(MarkdownBlock{
                        MarkdownBlockKind::Paragraph,
                        line,
                        0U,
                    });
                }
            }
        }

        if ( lineEnd == std::string_view::npos ) break;
        offset = lineEnd + 1U;
    }
}

/// @brief 逐片段访问一行 Markdown 行内内容，不分配内存。
/// @param text 已移除块级标记的单行文本。
/// @param visitor 接收 MarkdownInlineSpan 的回调。
/// @warning UI 可见路径：仅识别强调、行内代码和链接，不执行链接目标。
template<typename Visitor>
void visitMarkdownInline(std::string_view text, Visitor&& visitor)
{
    std::size_t offset = 0U;
    while ( offset < text.size() ) {
        if ( text[offset] == '\\' && offset + 1U < text.size() ) {
            visitor(MarkdownInlineSpan{
                MarkdownInlineKind::Text,
                text.substr(offset + 1U, 1U),
                {},
            });
            offset += 2U;
            continue;
        }

        if ( text[offset] == '`' ) {
            const auto close =
                MarkdownParserDetail::findUnescaped(text, '`', offset + 1U);
            if ( close != std::string_view::npos ) {
                visitor(MarkdownInlineSpan{
                    MarkdownInlineKind::Code,
                    text.substr(offset + 1U, close - offset - 1U),
                    {},
                });
                offset = close + 1U;
                continue;
            }
        }

        if ( text[offset] == '[' ) {
            const auto labelEnd =
                MarkdownParserDetail::findUnescaped(text, ']', offset + 1U);
            if ( labelEnd != std::string_view::npos &&
                 labelEnd + 1U < text.size() && text[labelEnd + 1U] == '(' ) {
                const auto destinationEnd = MarkdownParserDetail::findUnescaped(
                    text, ')', labelEnd + 2U);
                if ( destinationEnd != std::string_view::npos ) {
                    visitor(MarkdownInlineSpan{
                        MarkdownInlineKind::Link,
                        text.substr(offset + 1U, labelEnd - offset - 1U),
                        text.substr(labelEnd + 2U,
                                    destinationEnd - labelEnd - 2U),
                    });
                    offset = destinationEnd + 1U;
                    continue;
                }
            }
        }

        const bool strong =
            offset + 1U < text.size() && ((text.substr(offset, 2U) == "**") ||
                                          (text.substr(offset, 2U) == "__"));
        if ( strong ) {
            const auto marker = text.substr(offset, 2U);
            const auto close  = text.find(marker, offset + 2U);
            if ( close != std::string_view::npos ) {
                visitor(MarkdownInlineSpan{
                    MarkdownInlineKind::Strong,
                    text.substr(offset + 2U, close - offset - 2U),
                    {},
                });
                offset = close + 2U;
                continue;
            }
        }

        if ( text[offset] == '*' || text[offset] == '_' ) {
            const auto close = MarkdownParserDetail::findUnescaped(
                text, text[offset], offset + 1U);
            if ( close != std::string_view::npos ) {
                visitor(MarkdownInlineSpan{
                    MarkdownInlineKind::Emphasis,
                    text.substr(offset + 1U, close - offset - 1U),
                    {},
                });
                offset = close + 1U;
                continue;
            }
        }

        std::size_t next = offset + 1U;
        while ( next < text.size() && text[next] != '\\' && text[next] != '`' &&
                text[next] != '[' && text[next] != '*' && text[next] != '_' ) {
            ++next;
        }
        visitor(MarkdownInlineSpan{
            MarkdownInlineKind::Text,
            text.substr(offset, next - offset),
            {},
        });
        offset = next;
    }
}

}  // namespace MMM::UI
