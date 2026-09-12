#pragma once

#include <cctype>
#include <cstddef>
#include <string_view>

/// @file MarkdownParser.h
/// @brief 面向欢迎页安全 Markdown 子集的零分配、访问器式解析器。
/// @details 解析器返回的所有文本均为输入 string_view 的子视图，不拥有内存；
/// 调用者必须在访问器执行期间保证源字符串存活。
///
/// 块级语法范围：
/// - 一至六级 ATX 标题；
/// - 使用 -、* 或 + 的无序列表；
/// - 使用数字加 . 或 ) 的有序列表；
/// - 单行 > 引用；
/// - ``` 或 ~~~ 围栏代码块；
/// - 至少三个同类标记的分隔线；
/// - 空白行和普通单行段落。
///
/// 行内语法范围：
/// - 反斜杠转义的单字符；
/// - 反引号行内代码；
/// - 双星号或双下划线强强调；
/// - 单星号或单下划线强调；
/// - 方括号标签加圆括号目标的链接；
/// - 同语法前加感叹号的图片；
/// - 带嵌套括号、尖括号或可选标题的链接目标。
///
/// 明确不支持：
/// - HTML 标签和实体执行；
/// - 脚本、内联事件或网络访问；
/// - 引用式链接和脚注；
/// - 表格、任务列表和定义列表；
/// - 跨行段落合并或嵌套块 AST；
/// - 围栏语言的语法高亮；
/// - CommonMark 全量歧义消解。
///
/// 性能约定：
/// - 块解析对输入执行一次逐行扫描；
/// - 行内解析始终单调推进 offset；
/// - 不构造字符串、容器或语法树；
/// - visitor 同步接收轻量值对象；
/// - 不执行目标链接、图片加载或文件操作；
/// - 不在解析阶段进行本地化或文本测量。
/// - 不修改输入缓冲或缓存 visitor 返回值；
/// - 畸形标记作为普通文本继续解析；
/// - 所有循环都保证输入偏移单调前进。

namespace MMM::UI
{

/// @brief Markdown 块类型。
/// @note 解析器只覆盖欢迎页需要的安全子集，不构造 HTML 或脚本节点。
enum class MarkdownBlockKind {
    Paragraph,          ///< 普通单行段落。
    Heading,            ///< 一至六级 ATX 标题。
    UnorderedListItem,  ///< 使用 -、* 或 + 的无序列表项。
    OrderedListItem,    ///< 使用数字加点或右括号的有序列表项。
    Quote,              ///< 使用 > 前缀的引用行。
    Code,               ///< 围栏代码块内部的原始行。
    Separator,          ///< 至少三个同类标记组成的分隔线。
    Blank,              ///< 空白行。
};

/// @brief 单个 Markdown 块的零拥有解析结果。
struct MarkdownBlock {
    /// @brief 块类型。
    MarkdownBlockKind kind{ MarkdownBlockKind::Paragraph };
    /// @brief 已移除块级标记的正文。
    std::string_view text;
    /// @brief 标题级别或列表缩进层级。
    /// @note 其他块类型保持零。
    std::size_t level{ 0U };
};

/// @brief Markdown 行内样式类型。
/// @note 片段只描述显示语义，链接目标不会在解析阶段执行。
enum class MarkdownInlineKind {
    Text,      ///< 无样式文本或转义后的单字符。
    Strong,    ///< 双星号或双下划线强调。
    Emphasis,  ///< 单星号或单下划线强调。
    Code,      ///< 反引号包围的行内代码。
    Link,      ///< 带目标的普通链接。
    Image,     ///< 嵌入图片；目标不包含可选标题。
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
/// @param text 待处理的零拥有文本视图。
/// @return 与输入共享存储、移除左侧水平空白后的子视图。
inline std::string_view trimLeft(std::string_view text)
{
    // 只处理 Markdown 行首允许的空格和制表符，不移除其他 Unicode 空白。
    while ( !text.empty() && (text.front() == ' ' || text.front() == '\t') ) {
        text.remove_prefix(1U);
    }
    return text;
}

/// @brief 判断一行是否为 Markdown 分隔线。
/// @param line 待判断的单行文本。
/// @return 忽略水平空白后由至少三个相同 -、* 或 _ 组成时返回 true。
inline bool isSeparator(std::string_view line)
{
    // 允许分隔标记之间存在空格，先只移除行首缩进。
    line               = trimLeft(line);
    char        marker = '\0';
    std::size_t count  = 0U;
    for ( const char value : line ) {
        // 行内水平空白不参与标记计数。
        if ( value == ' ' || value == '\t' ) continue;
        // 第一个非空白字符确定整行必须使用的标记类型。
        if ( marker == '\0' ) marker = value;
        if ( value != marker ||
             (value != '-' && value != '*' && value != '_') ) {
            return false;
        }
        ++count;
    }
    // CommonMark 风格分隔线至少需要三个标记。
    return count >= 3U;
}

/// @brief 查找下一个未转义的指定字符。
/// @param text 待搜索文本。
/// @param needle 目标 ASCII 标记字符。
/// @param offset 搜索起始偏移。
/// @return 首个未被奇数个反斜杠转义的位置，未找到时返回 npos。
inline std::size_t findUnescaped(std::string_view text, char needle,
                                 std::size_t offset)
{
    while ( offset < text.size() ) {
        // 先利用 string_view 查找候选，再检查候选前的连续反斜杠。
        const auto found = text.find(needle, offset);
        if ( found == std::string_view::npos ) return found;
        std::size_t slashCount = 0U;
        // 偶数个反斜杠最终不转义标记，奇数个才表示转义。
        for ( std::size_t index = found; index > 0U && text[index - 1U] == '\\';
              --index ) {
            ++slashCount;
        }
        if ( slashCount % 2U == 0U ) return found;
        // 被转义候选之后继续搜索，不重新扫描已经检查的前缀。
        offset = found + 1U;
    }
    return std::string_view::npos;
}
}  // namespace MarkdownParserDetail

/// @brief 逐块访问 Markdown 文档，不分配内存。
/// @param markdown Markdown 源文本。
/// @param visitor 接收 MarkdownBlock 的回调。
/// @warning UI 可见路径：线性扫描输入文本，不执行 HTML、脚本、网络或文件操作。
/// @details 每个物理行产生至多一个块；围栏开始与结束行只切换状态，
/// 不交给 visitor。返回块中的 string_view 只在源 markdown 存活期间有效。
template<typename Visitor>
void visitMarkdownBlocks(std::string_view markdown, Visitor&& visitor)
{
    // codeBlock 仅表示是否位于 ``` 或 ~~~ 围栏内部，不解析语言标签。
    bool        codeBlock = false;
    std::size_t offset    = 0U;
    while ( offset <= markdown.size() ) {
        // 文档最后没有换行时 substr 直接延伸到末尾。
        const auto lineEnd = markdown.find('\n', offset);
        auto       line    = markdown.substr(offset,
                                             lineEnd == std::string_view::npos
                                                 ? std::string_view::npos
                                                 : lineEnd - offset);
        // 统一剥离 CRLF 中的 CR，块文本不包含平台换行符。
        if ( line.ends_with('\r') ) line.remove_suffix(1U);

        // 块级标记识别忽略左侧水平空白，但段落保留原始行内容。
        const auto trimmed = MarkdownParserDetail::trimLeft(line);
        if ( trimmed.starts_with("```") || trimmed.starts_with("~~~") ) {
            // 围栏标记只切换代码模式，不输出自身。
            codeBlock = !codeBlock;
        } else if ( codeBlock ) {
            // 代码块内部不解释标题、列表或行内 Markdown。
            visitor(MarkdownBlock{ MarkdownBlockKind::Code, line, 0U });
        } else if ( trimmed.empty() ) {
            // 空白块用于渲染器结束段落和插入垂直间距。
            visitor(MarkdownBlock{ MarkdownBlockKind::Blank, {}, 0U });
        } else if ( MarkdownParserDetail::isSeparator(trimmed) ) {
            // 分隔线不需要向渲染器传递标记文本。
            visitor(MarkdownBlock{ MarkdownBlockKind::Separator, {}, 0U });
        } else {
            // ATX 标题最多识别六个井号，并要求标记后紧跟空格。
            std::size_t headingLevel = 0U;
            while ( headingLevel < trimmed.size() && headingLevel < 6U &&
                    trimmed[headingLevel] == '#' ) {
                ++headingLevel;
            }
            if ( headingLevel > 0U && headingLevel < trimmed.size() &&
                 trimmed[headingLevel] == ' ' ) {
                // text 从标题标记和一个分隔空格之后开始。
                visitor(MarkdownBlock{
                    MarkdownBlockKind::Heading,
                    trimmed.substr(headingLevel + 1U),
                    headingLevel,
                });
            } else if ( trimmed.starts_with(">") ) {
                // 引用允许 > 后有一个可选空格。
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
                // 每两个行首空白字符折算为一级列表缩进。
                const std::size_t indent =
                    static_cast<std::size_t>(trimmed.data() - line.data()) / 2U;
                visitor(MarkdownBlock{
                    MarkdownBlockKind::UnorderedListItem,
                    trimmed.substr(2U),
                    indent,
                });
            } else {
                // 有序列表标记必须从一个或多个十进制数字开始。
                std::size_t numberEnd = 0U;
                while ( numberEnd < trimmed.size() &&
                        std::isdigit(
                            static_cast<unsigned char>(trimmed[numberEnd])) ) {
                    ++numberEnd;
                }
                if ( numberEnd > 0U && numberEnd + 1U < trimmed.size() &&
                     (trimmed[numberEnd] == '.' || trimmed[numberEnd] == ')') &&
                     trimmed[numberEnd + 1U] == ' ' ) {
                    // 保留原始数字标记，渲染器负责显示编号文本。
                    const std::size_t indent =
                        static_cast<std::size_t>(trimmed.data() - line.data()) /
                        2U;
                    visitor(MarkdownBlock{
                        MarkdownBlockKind::OrderedListItem,
                        trimmed,
                        indent,
                    });
                } else {
                    // 未命中任何受支持块语法时保留原始行作为段落。
                    visitor(MarkdownBlock{
                        MarkdownBlockKind::Paragraph,
                        line,
                        0U,
                    });
                }
            }
        }

        // 没有下一行时结束，避免 size()+1 溢出或重复空块。
        if ( lineEnd == std::string_view::npos ) break;
        offset = lineEnd + 1U;
    }
}

/// @brief 逐片段访问一行 Markdown 行内内容，不分配内存。
/// @param text 已移除块级标记的单行文本。
/// @param visitor 接收 MarkdownInlineSpan 的回调。
/// @warning UI 可见路径：仅识别强调、行内代码和链接，不执行链接目标。
/// @details 解析采用从左到右的优先级：转义、代码、链接或图片、强强调、
/// 单强调、普通文本。无法闭合的标记最终作为普通文本输出。
template<typename Visitor>
void visitMarkdownInline(std::string_view text, Visitor&& visitor)
{
    // offset 始终推进至少一个字符，保证畸形输入也能终止。
    std::size_t offset = 0U;
    while ( offset < text.size() ) {
        if ( text[offset] == '\\' && offset + 1U < text.size() ) {
            // 反斜杠本身不显示，下一个字符作为无样式文本片段输出。
            visitor(MarkdownInlineSpan{
                MarkdownInlineKind::Text,
                text.substr(offset + 1U, 1U),
                {},
            });
            offset += 2U;
            continue;
        }

        if ( text[offset] == '`' ) {
            // 行内代码查找下一个未转义反引号，不在内容中解析其他标记。
            const auto close =
                MarkdownParserDetail::findUnescaped(text, '`', offset + 1U);
            if ( close != std::string_view::npos ) {
                // 成功闭合后跳过两个边界标记。
                visitor(MarkdownInlineSpan{
                    MarkdownInlineKind::Code,
                    text.substr(offset + 1U, close - offset - 1U),
                    {},
                });
                offset = close + 1U;
                continue;
            }
        }

        // 图片语法比普通链接多一个前导感叹号，共享标签和目标解析。
        const bool image = text[offset] == '!' && offset + 1U < text.size() &&
                           text[offset + 1U] == '[';
        const auto labelStart = offset + (image ? 2U : 1U);
        if ( text[offset] == '[' || image ) {
            // 标签右括号必须未转义，后面必须立即跟目标左括号。
            const auto labelEnd =
                MarkdownParserDetail::findUnescaped(text, ']', labelStart);
            if ( labelEnd != std::string_view::npos &&
                 labelEnd + 1U < text.size() && text[labelEnd + 1U] == '(' ) {
                auto destinationEnd = labelEnd + 2U;
                // depth 支持目标 URL 内嵌套括号。
                unsigned depth = 1U;
                // quote 支持 URL 后的可选单引号或双引号标题。
                char quote = 0;
                for ( ; destinationEnd < text.size(); ++destinationEnd ) {
                    const char c = text[destinationEnd];
                    if ( c == '\\' && destinationEnd + 1U < text.size() ) {
                        // 跳过转义字符及其后继，避免错误改变括号深度。
                        ++destinationEnd;
                        continue;
                    }
                    if ( quote ) {
                        // 引号内部的括号不参与目标深度计算。
                        if ( c == quote ) quote = 0;
                        continue;
                    }
                    if ( (c == '"' || c == '\'') &&
                         destinationEnd > labelEnd + 2U &&
                         (text[destinationEnd - 1U] == ' ' ||
                          text[destinationEnd - 1U] == '\t') ) {
                        // 只有空白后的引号才视为可选标题起点。
                        quote = c;
                        continue;
                    }
                    if ( c == '(' ) ++depth;
                    if ( c == ')' && --depth == 0U ) break;
                }
                if ( destinationEnd == text.size() )
                    // 未闭合目标回退为普通文本，而不生成部分链接。
                    destinationEnd = std::string_view::npos;
                if ( destinationEnd != std::string_view::npos ) {
                    auto destination =
                        // 先移除目标开头水平空白，再解析尖括号或裸 URL。
                        MarkdownParserDetail::trimLeft(text.substr(
                            labelEnd + 2U, destinationEnd - labelEnd - 2U));
                    if ( destination.starts_with('<') ) {
                        // 尖括号目标允许空格，缺少右尖括号则生成空目标。
                        const auto end = destination.find('>');
                        destination    = end == std::string_view::npos
                                             ? std::string_view{}
                                             : destination.substr(1U, end - 1U);
                    } else {
                        // 裸目标在首个空白前结束，后续内容视为标题。
                        destination = destination.substr(
                            0, destination.find_first_of(" \t"));
                    }
                    visitor(MarkdownInlineSpan{
                        // 图片使用替代文本，普通链接使用可见标签。
                        image ? MarkdownInlineKind::Image
                              : MarkdownInlineKind::Link,
                        text.substr(labelStart, labelEnd - labelStart),
                        destination,
                    });
                    offset = destinationEnd + 1U;
                    continue;
                }
            }
        }

        // 强强调支持 ** 与 __ 两种成对标记。
        const bool strong =
            offset + 1U < text.size() && ((text.substr(offset, 2U) == "**") ||
                                          (text.substr(offset, 2U) == "__"));
        if ( strong ) {
            const auto marker = text.substr(offset, 2U);
            const auto close  = text.find(marker, offset + 2U);
            if ( close != std::string_view::npos ) {
                // 未闭合时不消费标记，交给末尾普通文本路径。
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
            // 单强调结束标记必须未被反斜杠转义。
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

        // 普通文本批量推进到下一个可能有语义的标记，减少 visitor 调用。
        std::size_t next = offset + 1U;
        while ( next < text.size() && text[next] != '\\' && text[next] != '`' &&
                text[next] != '[' && text[next] != '!' && text[next] != '*' &&
                text[next] != '_' ) {
            ++next;
        }
        visitor(MarkdownInlineSpan{
            MarkdownInlineKind::Text,
            text.substr(offset, next - offset),
            {},
        });
        // 即使当前字符是未闭合标记，next 也至少比 offset 大一。
        offset = next;
    }
}

}  // namespace MMM::UI
