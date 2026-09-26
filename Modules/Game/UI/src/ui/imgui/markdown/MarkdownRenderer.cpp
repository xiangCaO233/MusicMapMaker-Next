#include "ui/imgui/markdown/MarkdownRenderer.h"

#include "ui/imgui/markdown/MarkdownParser.h"
#include "ui/utils/DesktopPathUtils.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <string_view>

/// @file MarkdownRenderer.cpp
/// @brief 将安全 Markdown 子集直接排版到 ImDrawList 的无 AST 渲染实现。
/// @details 同一 MarkdownLayout 同时支持测量模式和绘制模式：drawList 为空时
/// 只推进布局光标，非空时提交文本与装饰几何，从而保持尺寸测量和实际绘制一致。
///
/// 排版约定：
/// - m_origin 使用屏幕坐标；
/// - x 与 lineY 使用相对排版区域坐标；
/// - m_width 至少为一个像素；
/// - m_maxHeight 小于等于零表示不限制高度；
/// - m_cursorY 记录当前文档占用高度；
/// - m_maxUsedWidth 记录实际最宽内容；
/// - 所有文本使用构造时捕获的当前 ImGui 字体；
/// - compact 只收紧行高和块间距，不改变语法。
///
/// 安全边界：
/// - 解析器不产生 HTML 或脚本节点；
/// - 链接只有 interactiveLinks 开启且存在绘制列表时可点击；
/// - 测量模式绝不执行链接；
/// - 图片只通过调用方提供的 IMarkdownImages 查询；
/// - 图片缓存未就绪时绘制有界占位区域；
/// - 外部 URL 打开只发生在用户明确点击命中区之后。
///
/// 换行约定：
/// - ASCII 连续非空白文本作为单词片段；
/// - 连续水平空白作为一个片段；
/// - 非 ASCII 文本按 UTF-8 码点拆分；
/// - 单词在当前行放不下时整体换行；
/// - 超宽单词再按码点递归拆分；
/// - 行首空白被丢弃；
/// - 高度上限一旦触发就停止后续块绘制。

namespace MMM::UI
{
namespace
{
/// @brief 返回下一个 UTF-8 码点的结束位置。
/// @param text UTF-8 字节视图。
/// @param offset 当前码点首字节偏移。
/// @return 根据首字节推断且不超过字符串末尾的下一偏移。
/// @note 畸形序列不做完整验证，只保证布局扫描单调前进且不越界。
std::size_t nextCodepointEnd(std::string_view text, std::size_t offset)
{
    // 已到末尾时保持末尾位置，供递归拆分安全终止。
    if ( offset >= text.size() ) return text.size();
    const auto  lead   = static_cast<unsigned char>(text[offset]);
    std::size_t length = 1U;
    // 按 UTF-8 首字节前缀推断 2、3、4 字节序列长度。
    if ( (lead & 0xE0U) == 0xC0U ) {
        length = 2U;
    } else if ( (lead & 0xF0U) == 0xE0U ) {
        length = 3U;
    } else if ( (lead & 0xF8U) == 0xF0U ) {
        length = 4U;
    }
    // 截断输入的末尾码点限制到实际缓冲末端。
    return std::min(text.size(), offset + length);
}

/// @brief 判断字符是否为布局空白。
/// @param value 待判断 ASCII 字节。
/// @return 空格或制表符返回 true。
bool isLayoutSpace(char value)
{
    return value == ' ' || value == '\t';
}

/// @brief 手工排版 Markdown 块到 ImDrawList。
class MarkdownLayout final
{
public:
    /// @brief 创建一次 Markdown 排版。
    /// @param drawList 目标绘制列表；为空时进入纯测量模式。
    /// @param origin 绘制区域屏幕坐标原点。
    /// @param width 可用换行宽度。
    /// @param maxHeight 可选高度上限，非正表示无限制。
    /// @param style 本次排版使用的颜色集合。
    /// @param compact 是否使用紧凑块间距。
    /// @param interactiveLinks 是否允许链接悬停和点击。
    /// @param images 可选非拥有图片查询接口。
    MarkdownLayout(
        ImDrawList* drawList, ImVec2 origin, float width, float maxHeight,
        const MarkdownStyle& style, bool compact, bool interactiveLinks,
        const IMarkdownImages*                       images,
        const std::function<void(std::string_view)>* onImageDoubleClick)
        : m_drawList(drawList)
        , m_origin(origin)
        , m_width(std::max(1.0F, width))
        , m_maxHeight(maxHeight)
        , m_style(style)
        , m_compact(compact)
        , m_interactiveLinks(interactiveLinks && drawList != nullptr)
        , m_images(images)
        , m_onImageDoubleClick(onImageDoubleClick)
        , m_font(ImGui::GetFont())
        , m_baseFontSize(ImGui::GetFontSize())
    {
        // 交互链接必须有真实绘制列表，否则测量过程不能产生副作用。
    }

    /// @brief 排版完整 Markdown 文档。
    /// @param markdown 待解析文档。
    /// @return 实际尺寸、截断状态和链接悬停状态。
    MarkdownLayoutResult run(std::string_view markdown)
    {
        // 达到高度上限后仍允许解析器结束扫描，但不再绘制后续块。
        visitMarkdownBlocks(markdown, [this](const MarkdownBlock& block) {
            if ( !m_truncated ) drawBlock(block);
        });
        // 报告宽度不超过约束宽度，高度来自最终纵向光标。
        return {
            .size        = { std::min(m_width, m_maxUsedWidth), m_cursorY },
            .truncated   = m_truncated,
            .linkHovered = m_linkHovered,
        };
    }

private:
    /// @brief 绘制单个块。
    /// @param block 已移除块级标记的零拥有解析结果。
    void drawBlock(const MarkdownBlock& block)
    {
        // 紧凑模式统一缩小段落后间距。
        const float paragraphGap = m_compact ? 2.0F : 5.0F;
        switch ( block.kind ) {
        case MarkdownBlockKind::Blank:
            // 空行只推进部分字号高度，不提交可见几何。
            advance(m_baseFontSize * (m_compact ? 0.32F : 0.5F));
            return;
        case MarkdownBlockKind::Separator: {
            // 分隔线先预留完整高度，再绘制在区域垂直中心。
            const float height = m_compact ? 5.0F : 9.0F;
            if ( !reserve(height) ) return;
            if ( m_drawList ) {
                // 测量模式跳过 DrawList 调用但保持相同高度。
                const float lineY = m_origin.y + m_cursorY + height * 0.5F;
                m_drawList->AddLine({ m_origin.x, lineY },
                                    { m_origin.x + m_width, lineY },
                                    m_style.accentColor,
                                    1.0F);
            }
            advance(height);
            return;
        }
        case MarkdownBlockKind::Heading: {
            // 非文档首块标题前增加间距，避免连续内容贴合。
            if ( m_cursorY > 0.0F ) advance(m_compact ? 2.0F : 4.0F);
            // 一级、二级和其余标题使用三档字号。
            const float scale = block.level <= 1U   ? 1.38F
                                : block.level == 2U ? 1.22F
                                                    : 1.08F;
            drawInline(block.text, 0.0F, scale, m_style.strongColor, true);
            advance(m_compact ? 1.0F : 3.0F);
            return;
        }
        case MarkdownBlockKind::UnorderedListItem: {
            // 列表缩进由解析层级决定，圆点和文字间保留固定间距。
            const float indent = 8.0F + static_cast<float>(block.level) * 12.0F;
            if ( !reserve(m_baseFontSize) ) return;
            if ( m_drawList ) {
                // 圆点对齐当前首行基线附近，换行文本继续沿文字 inset。
                const ImVec2 center{ m_origin.x + indent,
                                     m_origin.y + m_cursorY +
                                         m_baseFontSize * 0.48F };
                m_drawList->AddCircleFilled(center, 2.2F, m_style.accentColor);
            }
            drawInline(
                block.text, indent + 9.0F, 1.0F, m_style.textColor, false);
            advance(paragraphGap);
            return;
        }
        case MarkdownBlockKind::OrderedListItem: {
            // 解析器保留原始数字标记，因此整行从层级缩进处排版。
            const float indent = static_cast<float>(block.level) * 12.0F;
            drawInline(block.text, indent, 1.0F, m_style.textColor, false);
            advance(paragraphGap);
            return;
        }
        case MarkdownBlockKind::Quote: {
            // 引用正文右移，左侧强调线覆盖正文实际占用高度。
            constexpr float QUOTE_INSET = 10.0F;
            const float     startY      = m_cursorY;
            drawInline(
                block.text, QUOTE_INSET, 1.0F, m_style.mutedColor, false);
            if ( m_drawList && m_cursorY > startY ) {
                // 只有正文成功占用空间时绘制引用竖线。
                m_drawList->AddLine(
                    { m_origin.x + 2.0F, m_origin.y + startY },
                    { m_origin.x + 2.0F, m_origin.y + m_cursorY - 1.0F },
                    m_style.accentColor,
                    2.0F);
            }
            advance(paragraphGap);
            return;
        }
        case MarkdownBlockKind::Code: {
            // 首次排版用于确定实际高度，背景必须在文字之前绘制。
            const float startY = m_cursorY;
            drawInline(
                block.text, 6.0F, 1.0F, m_style.codeTextColor, false, true);
            if ( m_drawList && m_cursorY > startY ) {
                // 先覆盖整行背景，再从原始起点重绘代码文字。
                const ImVec2 min{ m_origin.x, m_origin.y + startY - 1.0F };
                const ImVec2 max{ m_origin.x + m_width,
                                  m_origin.y + m_cursorY };
                m_drawList->AddRectFilled(
                    min, max, m_style.codeBackgroundColor, 3.0F);
                drawInlineAt(block.text,
                             6.0F,
                             startY,
                             1.0F,
                             m_style.codeTextColor,
                             false,
                             true);
            }
            advance(m_compact ? 1.0F : 2.0F);
            return;
        }
        case MarkdownBlockKind::Paragraph:
            // 普通段落使用基础字号和正文颜色。
            drawInline(block.text, 0.0F, 1.0F, m_style.textColor, false);
            advance(paragraphGap);
            return;
        }
    }

    /// @brief 从当前纵坐标排版一行块内容。
    /// @param text 已移除块级标记的单行内容。
    /// @param inset 块级左侧缩进。
    /// @param fontScale 相对当前 ImGui 字号倍率。
    /// @param baseColor 普通文本颜色。
    /// @param forceStrong 是否把全部片段按强强调绘制。
    /// @param forceCode 是否绕过行内解析并按代码绘制。
    void drawInline(std::string_view text, float inset, float fontScale,
                    ImU32 baseColor, bool forceStrong, bool forceCode = false)
    {
        // 保存当前纵向光标作为首行起点，实际排版委托给可指定起点的入口。
        const float startY = m_cursorY;
        drawInlineAt(
            text, inset, startY, fontScale, baseColor, forceStrong, forceCode);
    }

    /// @brief 从指定纵坐标排版一行块内容。
    /// @param text 待排版行内 Markdown。
    /// @param inset 左侧缩进。
    /// @param startY 首行相对纵坐标。
    /// @param fontScale 字号倍率。
    /// @param baseColor 默认颜色。
    /// @param forceStrong 是否强制粗体模拟。
    /// @param forceCode 是否把整行按代码处理。
    void drawInlineAt(std::string_view text, float inset, float startY,
                      float fontScale, ImU32 baseColor, bool forceStrong,
                      bool forceCode)
    {
        // 已截断文档不能再次扩展尺寸或提交绘制命令。
        if ( m_truncated ) return;
        const float fontSize   = m_baseFontSize * fontScale;
        const float lineHeight = fontSize + (m_compact ? 1.0F : 2.0F);
        float       x          = inset;
        float       lineY      = startY;
        // 首行本身必须先通过高度预算检查。
        if ( !reserveAt(lineY, lineHeight) ) return;

        auto drawSpan = [&](const MarkdownInlineSpan& span) {
            // 单个 span 处理期间也可能因图片或换行触发截断。
            if ( m_truncated ) return;
            // 围栏代码块强制覆盖解析器可能产生的行内样式。
            const auto kind = forceCode ? MarkdownInlineKind::Code : span.kind;
            if ( kind == MarkdownInlineKind::Image ) {
                // 图片通过非拥有接口查询，未提供缓存时使用默认加载中布局。
                const auto image = m_images
                                       ? m_images->findImage(span.destination)
                                       : MarkdownImage{};
                // 图片始终从新行开始，避免与前置文字横向混排。
                if ( x > inset ) lineY += lineHeight;
                x = inset;
                // 图片替代文字始终作为标签展示，不再仅在加载失败时可见。
                if ( !span.text.empty() ) {
                    // 替代文字始终显示，既提供语义也说明加载失败图片内容。
                    visitTextPieces(span.text, [&](std::string_view piece) {
                        placePiece(piece,
                                   {},
                                   inset,
                                   x,
                                   lineY,
                                   fontSize,
                                   lineHeight,
                                   m_style.textColor,
                                   false,
                                   false,
                                   false);
                    });
                    if ( m_truncated ) return;
                    // 替代文字与图片之间保留四像素间距。
                    lineY += lineHeight + 4.0F;
                    x = inset;
                }
                // 图片宽度不超过当前块可用宽度，并保持原始帧宽高比。
                const float available = std::max(1.0F, m_width - inset);
                const float width     = image.size.x > 0
                                            ? std::min(available, image.size.x)
                                            : available;
                // 未知尺寸使用三行高度占位，避免布局在加载后完全塌陷。
                const float height = image.size.x > 0
                                         ? width * image.size.y / image.size.x
                                         : lineHeight * 3;
                if ( !reserveAt(lineY, height) ) return;
                const ImVec2 min{ m_origin.x + inset, m_origin.y + lineY };
                const ImVec2 max{ min.x + width, min.y + height };
                if ( m_drawList ) {
                    if ( image.texture ) {
                        // 已就绪纹理使用缓存提供的当前动画帧 UV。
                        m_drawList->AddImage(
                            image.texture, min, max, image.uv0, image.uv1);
                        // 只对当前可交互窗口内的真实图片响应双击，测量模式不触发回调。
                        if ( m_onImageDoubleClick && *m_onImageDoubleClick &&
                             ImGui::IsWindowHovered() &&
                             ImGui::IsMouseHoveringRect(min, max, true) ) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            if ( ImGui::IsMouseDoubleClicked(
                                     ImGuiMouseButton_Left) )
                                (*m_onImageDoubleClick)(span.destination);
                        }
                    } else {
                        // 排队、失败或没有缓存接口时绘制统一占位块。
                        m_drawList->AddRectFilled(
                            min, max, m_style.codeBackgroundColor);
                        // 状态文本区分加载失败、正在加载和未提供图片服务。
                        const char* status =
                            image.failed ? "图片加载失败"
                                         : (m_images ? "图片加载中…" : "图片");
                        m_drawList->AddText(min, m_style.mutedColor, status);
                    }
                }
                // 图片宽度参与文档实际最大宽度统计。
                m_maxUsedWidth = std::max(m_maxUsedWidth, inset + width);
                lineY += height;
                return;
            }
            // 普通 span 从块级默认样式开始，再按行内种类覆盖。
            ImU32 color  = baseColor;
            bool  strong = forceStrong;
            bool  code   = false;
            bool  link   = false;
            if ( kind == MarkdownInlineKind::Strong ) {
                // ImGui 无独立粗体字体时使用颜色和轻微重复绘制模拟强调。
                color  = m_style.strongColor;
                strong = true;
            } else if ( kind == MarkdownInlineKind::Emphasis ) {
                // 斜体字体不可用时以 mutedColor 表达轻强调。
                color = m_style.mutedColor;
            } else if ( kind == MarkdownInlineKind::Code ) {
                // 行内代码启用专用文字色和每片段背景。
                color = m_style.codeTextColor;
                code  = true;
            } else if ( kind == MarkdownInlineKind::Link ) {
                // 链接使用专用颜色、下划线和可选交互命中区。
                color = m_style.linkColor;
                link  = true;
            }

            // 文本片段进一步按换行单元切分后逐个放置。
            visitTextPieces(span.text, [&](std::string_view piece) {
                placePiece(piece,
                           span.destination,
                           inset,
                           x,
                           lineY,
                           fontSize,
                           lineHeight,
                           color,
                           strong,
                           code,
                           link);
            });
        };

        if ( forceCode ) {
            // 围栏代码行不解析内部 Markdown 标记。
            drawSpan({ MarkdownInlineKind::Code, text, {} });
        } else {
            // 普通块先解析行内样式，再进入统一 span 布局。
            visitMarkdownInline(text, drawSpan);
        }
        // 至少计入最后一行高度，即使该行只含空白。
        m_cursorY = std::max(m_cursorY, lineY + lineHeight);
        if ( m_maxHeight > 0.0F ) {
            // 截断边界下报告高度不得超过调用方上限。
            m_cursorY = std::min(m_cursorY, m_maxHeight);
        }
        m_maxUsedWidth = std::max(m_maxUsedWidth, x);
    }

    /// @brief 按空白、ASCII 单词与 UTF-8 码点切分布局片段。
    /// @param text 待切分文本。
    /// @param visitor 同步接收每个零拥有片段。
    /// @note 切分只服务换行，不改变字节内容或 Unicode 规范化。
    template<typename Visitor>
    static void visitTextPieces(std::string_view text, Visitor&& visitor)
    {
        // offset 按片段末尾单调推进，整个扫描为线性复杂度。
        std::size_t offset = 0U;
        while ( offset < text.size() ) {
            const auto byte = static_cast<unsigned char>(text[offset]);
            if ( isLayoutSpace(text[offset]) ) {
                // 连续水平空白合并，行首时可一次丢弃。
                std::size_t end = offset + 1U;
                while ( end < text.size() && isLayoutSpace(text[end]) ) ++end;
                visitor(text.substr(offset, end - offset));
                offset = end;
            } else if ( byte >= 0x80U ) {
                // 非 ASCII 按单个 UTF-8 码点提供，支持无空格语言逐字换行。
                const auto end = nextCodepointEnd(text, offset);
                visitor(text.substr(offset, end - offset));
                offset = end;
            } else {
                // ASCII 非空白连续到下一空白或非 ASCII 字节，视为一个单词。
                std::size_t end = offset + 1U;
                while ( end < text.size() && !isLayoutSpace(text[end]) &&
                        static_cast<unsigned char>(text[end]) < 0x80U ) {
                    ++end;
                }
                visitor(text.substr(offset, end - offset));
                offset = end;
            }
        }
    }

    /// @brief 放置一个文字片段并在必要时换行。
    /// @param piece 当前空白、ASCII 单词或 UTF-8 码点片段。
    /// @param destination 链接目标，仅 link 为 true 时使用。
    /// @param inset 当前块左侧缩进。
    /// @param x 当前行横向光标，调用后推进。
    /// @param lineY 当前行纵坐标，换行时推进。
    /// @param fontSize 绘制字号。
    /// @param lineHeight 当前行高。
    /// @param color 文本与链接下划线颜色。
    /// @param strong 是否重复绘制模拟粗体。
    /// @param code 是否绘制代码背景。
    /// @param link 是否绘制下划线并处理交互。
    void placePiece(std::string_view piece, std::string_view destination,
                    float inset, float& x, float& lineY, float fontSize,
                    float lineHeight, ImU32 color, bool strong, bool code,
                    bool link)
    {
        // 空片段和已截断状态不改变布局。
        if ( piece.empty() || m_truncated ) return;
        const bool  whitespace     = isLayoutSpace(piece.front());
        const float pieceWidth     = textWidth(piece, fontSize);
        const float availableWidth = std::max(1.0F, m_width - inset);

        // 非空白片段放不下且当前行已有内容时先整体换行。
        if ( !whitespace && x > inset && x + pieceWidth > m_width ) {
            x = inset;
            lineY += lineHeight;
            if ( !reserveAt(lineY, lineHeight) ) return;
        }
        // 丢弃每行开头的空白，避免换行后产生视觉缩进。
        if ( whitespace && x <= inset + 0.01F ) return;

        if ( !whitespace && pieceWidth > availableWidth && piece.size() > 1U ) {
            // 超宽单词无法整体放入任何行时按 UTF-8 码点递归拆分。
            std::size_t offset = 0U;
            while ( offset < piece.size() && !m_truncated ) {
                const auto end = nextCodepointEnd(piece, offset);
                placePiece(piece.substr(offset, end - offset),
                           destination,
                           inset,
                           x,
                           lineY,
                           fontSize,
                           lineHeight,
                           color,
                           strong,
                           code,
                           link);
                offset = end;
            }
            // 子片段已经独立推进 x 和 lineY，父片段无需再次计宽。
            return;
        }

        if ( code && m_drawList && !whitespace ) {
            // 行内代码背景略微扩展到文字左右和基线外侧。
            m_drawList->AddRectFilled(
                { m_origin.x + x - 2.0F, m_origin.y + lineY - 1.0F },
                { m_origin.x + x + pieceWidth + 2.0F,
                  m_origin.y + lineY + lineHeight },
                m_style.codeBackgroundColor,
                2.0F);
        }
        if ( m_drawList && !whitespace ) {
            // 空白只推进 x，不生成独立文字 draw command。
            const ImVec2 position{ m_origin.x + x, m_origin.y + lineY };
            m_drawList->AddText(m_font,
                                fontSize,
                                position,
                                color,
                                piece.data(),
                                piece.data() + piece.size());
            if ( strong ) {
                // 横向偏移 0.45 像素的第二次绘制模拟更粗字重。
                m_drawList->AddText(m_font,
                                    fontSize,
                                    { position.x + 0.45F, position.y },
                                    color,
                                    piece.data(),
                                    piece.data() + piece.size());
            }
            if ( link ) {
                // 下划线位于字号底部，并以文字边界建立鼠标命中矩形。
                const float underlineY = position.y + fontSize;
                m_drawList->AddLine({ position.x, underlineY },
                                    { position.x + pieceWidth, underlineY },
                                    color,
                                    1.0F);
                handleLinkInteraction(
                    position,
                    { position.x + pieceWidth, position.y + lineHeight },
                    destination);
            }
        }
        // 所有片段包括中间空白都推进横向光标。
        x += pieceWidth;
        m_maxUsedWidth = std::max(m_maxUsedWidth, x);
    }

    /// @brief 处理链接悬浮与点击。
    /// @param min 链接片段屏幕坐标左上角。
    /// @param max 链接片段屏幕坐标右下角。
    /// @param destination 待交给桌面浏览器的目标。
    /// @warning UI 热路径：只有用户点击命中链接时才触发外部打开操作。
    void handleLinkInteraction(const ImVec2& min, const ImVec2& max,
                               std::string_view destination)
    {
        // 同时要求启用交互、非空目标、当前窗口 hover 和矩形命中。
        if ( !m_interactiveLinks || destination.empty() ||
             !ImGui::IsWindowHovered() ||
             !ImGui::IsMouseHoveringRect(min, max, true) ) {
            return;
        }
        // 汇总任一片段悬停状态供调用方调整外部交互策略。
        m_linkHovered = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if ( ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
            // 仅明确左键点击后委托平台工具打开 URL。
            DesktopPathUtils::openUrlInBrowser(destination);
        }
    }

    /// @brief 计算指定字号的文本宽度。
    /// @param text 待测量字节区间。
    /// @param fontSize 目标字号。
    /// @return 当前字体下不换行的像素宽度。
    float textWidth(std::string_view text, float fontSize) const
    {
        return m_font
            ->CalcTextSizeA(
                fontSize, FLT_MAX, 0.0F, text.data(), text.data() + text.size())
            .x;
    }

    /// @brief 检查当前纵坐标能否容纳指定高度。
    /// @param height 待预留高度。
    /// @return 未超过高度限制时返回 true。
    bool reserve(float height) { return reserveAt(m_cursorY, height); }

    /// @brief 检查指定纵坐标能否容纳指定高度。
    /// @param y 相对排版原点的纵坐标。
    /// @param height 待预留高度。
    /// @return 可容纳时返回 true；否则设置截断状态并返回 false。
    bool reserveAt(float y, float height)
    {
        if ( m_maxHeight > 0.0F && y + height > m_maxHeight + 0.01F ) {
            // 容差避免精确边界上的浮点误差误判为截断。
            m_cursorY   = std::min(m_cursorY, m_maxHeight);
            m_truncated = true;
            return false;
        }
        return true;
    }

    /// @brief 推进纵向布局光标。
    /// @param distance 待增加的块间距或高度。
    void advance(float distance)
    {
        // 非正距离或截断后推进均保持无操作。
        if ( distance <= 0.0F || m_truncated ) return;
        if ( m_maxHeight > 0.0F && m_cursorY + distance > m_maxHeight ) {
            // 推进跨过上限时钳制光标并停止后续布局。
            m_cursorY   = m_maxHeight;
            m_truncated = true;
            return;
        }
        m_cursorY += distance;
    }

    /// @brief 目标绘制列表；测量模式为空。
    ImDrawList* m_drawList{ nullptr };
    /// @brief 绘制区域左上角。
    ImVec2 m_origin;
    /// @brief 可用排版宽度。
    float m_width{ 1.0F };
    /// @brief 可选最大高度。
    float m_maxHeight{ 0.0F };
    /// @brief 本次排版配色。
    MarkdownStyle m_style;
    /// @brief 是否使用紧凑间距。
    bool m_compact{ false };
    /// @brief 是否允许链接点击。
    bool m_interactiveLinks{ false };
    /// @brief 本次排版使用的非拥有图片缓存。
    const IMarkdownImages* m_images{ nullptr };
    /// @brief 仅在绘制模式调用的图片双击回调，所有权留在本帧调用方。
    const std::function<void(std::string_view)>* m_onImageDoubleClick{
        nullptr
    };
    /// @brief 当前 ImGui 字体。
    ImFont* m_font{ nullptr };
    /// @brief 当前 ImGui 基础字号。
    float m_baseFontSize{ 0.0F };
    /// @brief 当前纵向布局光标。
    float m_cursorY{ 0.0F };
    /// @brief 已使用的最大横向范围。
    float m_maxUsedWidth{ 0.0F };
    /// @brief 是否已经到达高度上限。
    bool m_truncated{ false };
    /// @brief 当前鼠标是否悬浮于链接。
    bool m_linkHovered{ false };
};

/// @brief 解析当前选项使用的宽度。
/// @param options 调用方渲染选项。
/// @return 显式正 wrapWidth，或当前 ImGui 内容区至少一像素的可用宽度。
float resolveWrapWidth(const MarkdownRenderOptions& options)
{
    // 显式宽度优先，允许测量和绘制使用相同固定排版宽度。
    if ( options.wrapWidth > 0.0F ) return options.wrapWidth;
    // 自动宽度只在存在当前 ImGui 窗口的调用路径使用。
    return std::max(1.0F, ImGui::GetContentRegionAvail().x);
}
}  // namespace

/// @brief 从当前 ImGui 主题构造 Markdown 默认配色。
/// @return 正文、强调、链接、代码和装饰颜色值快照。
/// @warning UI 热路径：只读取当前样式，不修改样式栈。
MarkdownStyle defaultMarkdownStyle()
{
    // 正文和强调沿用主题文字色，轻文本沿用禁用文字色。
    return {
        .textColor   = ImGui::GetColorU32(ImGuiCol_Text),
        .strongColor = ImGui::GetColorU32(ImGuiCol_Text),
        .mutedColor  = ImGui::GetColorU32(ImGuiCol_TextDisabled),
        // 链接使用固定易识别蓝色，独立于主题强调色。
        .linkColor =
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.38F, 0.68F, 1.0F, 1.0F)),
        .codeTextColor =
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.82F, 0.88F, 0.96F, 1.0F)),
        // 代码背景和分隔装饰复用主题 FrameBg 与 Border。
        .codeBackgroundColor = ImGui::GetColorU32(ImGuiCol_FrameBg),
        .accentColor         = ImGui::GetColorU32(ImGuiCol_Border),
    };
}

/// @brief 在不提交 DrawList 命令的情况下测量 Markdown 布局。
/// @param markdown 待测量文档。
/// @param options 宽度、高度、紧凑模式、样式和图片查询选项。
/// @return 与真实渲染一致的尺寸和截断状态；linkHovered 始终为 false。
MarkdownLayoutResult measureMarkdown(std::string_view             markdown,
                                     const MarkdownRenderOptions& options)
{
    // 自定义样式按值复制；未提供时从当前 ImGui 主题生成快照。
    const MarkdownStyle style =
        options.style ? *options.style : defaultMarkdownStyle();
    // 空 drawList 强制关闭链接交互，同时复用全部排版算法。
    MarkdownLayout layout(nullptr,
                          { 0.0F, 0.0F },
                          resolveWrapWidth(options),
                          options.maxHeight,
                          style,
                          options.compact,
                          false,
                          options.images,
                          nullptr);
    return layout.run(markdown);
}

/// @brief 在当前 ImGui 光标处绘制 Markdown 并占据对应布局空间。
/// @param markdown 待绘制文档。
/// @param options 渲染宽度、高度、样式、交互和图片选项。
/// @warning UI 热路径：线性解析并提交可见绘制命令，不访问文件或网络。
void renderMarkdown(std::string_view             markdown,
                    const MarkdownRenderOptions& options)
{
    // 样式在整次排版期间固定，防止中途主题变化产生混合颜色。
    const MarkdownStyle style =
        options.style ? *options.style : defaultMarkdownStyle();
    // 当前光标屏幕位置作为手工 DrawList 坐标原点。
    const float    width  = resolveWrapWidth(options);
    const ImVec2   origin = ImGui::GetCursorScreenPos();
    MarkdownLayout layout(ImGui::GetWindowDrawList(),
                          origin,
                          width,
                          options.maxHeight,
                          style,
                          options.compact,
                          options.interactiveLinks,
                          options.images,
                          &options.onImageDoubleClick);
    // Dummy 把手工绘制占用面积反馈给 ImGui 后续布局和滚动计算。
    const auto result = layout.run(markdown);
    ImGui::Dummy({ width, std::max(1.0F, result.size.y) });
}

/// @brief 在显式矩形内绘制 Markdown，不改变当前 ImGui 布局光标。
/// @param drawList 目标绘制列表。
/// @param min 裁剪矩形左上角。
/// @param max 裁剪矩形右下角。
/// @param markdown 待绘制文档。
/// @param options 渲染选项；verticalOffset 用于外部滚动。
/// @return 完整内容尺寸、截断和链接悬停状态。
/// @warning UI 热路径：调用方负责用返回尺寸维护滚动范围。
MarkdownLayoutResult renderMarkdownToDrawList(
    ImDrawList& drawList, const ImVec2& min, const ImVec2& max,
    std::string_view markdown, const MarkdownRenderOptions& options)
{
    // 显式矩形保证宽高至少一像素，避免退化裁剪和除法。
    const MarkdownStyle style =
        options.style ? *options.style : defaultMarkdownStyle();
    const float rectWidth  = std::max(1.0F, max.x - min.x);
    const float rectHeight = std::max(1.0F, max.y - min.y);
    // 显式 wrapWidth 仍不能超过目标矩形宽度。
    const float width = options.wrapWidth > 0.0F
                            ? std::min(options.wrapWidth, rectWidth)
                            : rectWidth;
    // 非有限或负滚动偏移按零处理。
    const float verticalOffset = std::max(
        0.0F,
        std::isfinite(options.verticalOffset) ? options.verticalOffset : 0.0F);
    // 排版原点上移后高度预算需补回偏移，才能覆盖当前可见底部。
    const float maxHeight =
        options.maxHeight > 0.0F
            ? std::min(options.maxHeight, rectHeight) + verticalOffset
            : rectHeight + verticalOffset;
    // 所有手工文字、背景和图片严格裁剪到调用方矩形。
    drawList.PushClipRect(min, max, true);
    MarkdownLayout layout(&drawList,
                          { min.x, min.y - verticalOffset },
                          width,
                          maxHeight,
                          style,
                          options.compact,
                          options.interactiveLinks,
                          options.images,
                          &options.onImageDoubleClick);
    // 返回的是完整排版坐标结果，调用方可继续计算滚动条。
    const auto result = layout.run(markdown);
    drawList.PopClipRect();
    return result;
}

}  // namespace MMM::UI
