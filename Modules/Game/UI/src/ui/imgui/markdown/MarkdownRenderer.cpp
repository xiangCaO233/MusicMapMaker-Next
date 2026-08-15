#include "ui/imgui/markdown/MarkdownRenderer.h"

#include "ui/imgui/markdown/MarkdownParser.h"
#include "ui/utils/DesktopPathUtils.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace MMM::UI
{
namespace
{
/// @brief 返回下一个 UTF-8 码点的结束位置。
std::size_t nextCodepointEnd(std::string_view text, std::size_t offset)
{
    if ( offset >= text.size() ) return text.size();
    const auto  lead   = static_cast<unsigned char>(text[offset]);
    std::size_t length = 1U;
    if ( (lead & 0xE0U) == 0xC0U ) {
        length = 2U;
    } else if ( (lead & 0xF0U) == 0xE0U ) {
        length = 3U;
    } else if ( (lead & 0xF8U) == 0xF0U ) {
        length = 4U;
    }
    return std::min(text.size(), offset + length);
}

/// @brief 判断字符是否为布局空白。
bool isLayoutSpace(char value)
{
    return value == ' ' || value == '\t';
}

/// @brief 手工排版 Markdown 块到 ImDrawList。
class MarkdownLayout final
{
public:
    /// @brief 创建一次 Markdown 排版。
    MarkdownLayout(ImDrawList* drawList, ImVec2 origin, float width,
                   float maxHeight, const MarkdownStyle& style, bool compact,
                   bool interactiveLinks)
        : m_drawList(drawList)
        , m_origin(origin)
        , m_width(std::max(1.0F, width))
        , m_maxHeight(maxHeight)
        , m_style(style)
        , m_compact(compact)
        , m_interactiveLinks(interactiveLinks && drawList != nullptr)
        , m_font(ImGui::GetFont())
        , m_baseFontSize(ImGui::GetFontSize())
    {
    }

    /// @brief 排版完整 Markdown 文档。
    MarkdownLayoutResult run(std::string_view markdown)
    {
        visitMarkdownBlocks(markdown, [this](const MarkdownBlock& block) {
            if ( !m_truncated ) drawBlock(block);
        });
        return {
            .size        = { std::min(m_width, m_maxUsedWidth), m_cursorY },
            .truncated   = m_truncated,
            .linkHovered = m_linkHovered,
        };
    }

private:
    /// @brief 绘制单个块。
    void drawBlock(const MarkdownBlock& block)
    {
        const float paragraphGap = m_compact ? 2.0F : 5.0F;
        switch ( block.kind ) {
        case MarkdownBlockKind::Blank:
            advance(m_baseFontSize * (m_compact ? 0.32F : 0.5F));
            return;
        case MarkdownBlockKind::Separator: {
            const float height = m_compact ? 5.0F : 9.0F;
            if ( !reserve(height) ) return;
            if ( m_drawList ) {
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
            if ( m_cursorY > 0.0F ) advance(m_compact ? 2.0F : 4.0F);
            const float scale = block.level <= 1U   ? 1.38F
                                : block.level == 2U ? 1.22F
                                                    : 1.08F;
            drawInline(block.text, 0.0F, scale, m_style.strongColor, true);
            advance(m_compact ? 1.0F : 3.0F);
            return;
        }
        case MarkdownBlockKind::UnorderedListItem: {
            const float indent = 8.0F + static_cast<float>(block.level) * 12.0F;
            if ( !reserve(m_baseFontSize) ) return;
            if ( m_drawList ) {
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
            const float indent = static_cast<float>(block.level) * 12.0F;
            drawInline(block.text, indent, 1.0F, m_style.textColor, false);
            advance(paragraphGap);
            return;
        }
        case MarkdownBlockKind::Quote: {
            constexpr float QUOTE_INSET = 10.0F;
            const float     startY      = m_cursorY;
            drawInline(
                block.text, QUOTE_INSET, 1.0F, m_style.mutedColor, false);
            if ( m_drawList && m_cursorY > startY ) {
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
            const float startY = m_cursorY;
            drawInline(
                block.text, 6.0F, 1.0F, m_style.codeTextColor, false, true);
            if ( m_drawList && m_cursorY > startY ) {
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
            drawInline(block.text, 0.0F, 1.0F, m_style.textColor, false);
            advance(paragraphGap);
            return;
        }
    }

    /// @brief 从当前纵坐标排版一行块内容。
    void drawInline(std::string_view text, float inset, float fontScale,
                    ImU32 baseColor, bool forceStrong, bool forceCode = false)
    {
        const float startY = m_cursorY;
        drawInlineAt(
            text, inset, startY, fontScale, baseColor, forceStrong, forceCode);
    }

    /// @brief 从指定纵坐标排版一行块内容。
    void drawInlineAt(std::string_view text, float inset, float startY,
                      float fontScale, ImU32 baseColor, bool forceStrong,
                      bool forceCode)
    {
        if ( m_truncated ) return;
        const float fontSize   = m_baseFontSize * fontScale;
        const float lineHeight = fontSize + (m_compact ? 1.0F : 2.0F);
        float       x          = inset;
        float       lineY      = startY;
        if ( !reserveAt(lineY, lineHeight) ) return;

        auto drawSpan = [&](const MarkdownInlineSpan& span) {
            const auto kind  = forceCode ? MarkdownInlineKind::Code : span.kind;
            ImU32      color = baseColor;
            bool       strong = forceStrong;
            bool       code   = false;
            bool       link   = false;
            if ( kind == MarkdownInlineKind::Strong ) {
                color  = m_style.strongColor;
                strong = true;
            } else if ( kind == MarkdownInlineKind::Emphasis ) {
                color = m_style.mutedColor;
            } else if ( kind == MarkdownInlineKind::Code ) {
                color = m_style.codeTextColor;
                code  = true;
            } else if ( kind == MarkdownInlineKind::Link ) {
                color = m_style.linkColor;
                link  = true;
            }

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
            drawSpan({ MarkdownInlineKind::Code, text, {} });
        } else {
            visitMarkdownInline(text, drawSpan);
        }
        m_cursorY = std::max(m_cursorY, lineY + lineHeight);
        if ( m_maxHeight > 0.0F ) {
            m_cursorY = std::min(m_cursorY, m_maxHeight);
        }
        m_maxUsedWidth = std::max(m_maxUsedWidth, x);
    }

    /// @brief 按空白、ASCII 单词与 UTF-8 码点切分布局片段。
    template<typename Visitor>
    static void visitTextPieces(std::string_view text, Visitor&& visitor)
    {
        std::size_t offset = 0U;
        while ( offset < text.size() ) {
            const auto byte = static_cast<unsigned char>(text[offset]);
            if ( isLayoutSpace(text[offset]) ) {
                std::size_t end = offset + 1U;
                while ( end < text.size() && isLayoutSpace(text[end]) ) ++end;
                visitor(text.substr(offset, end - offset));
                offset = end;
            } else if ( byte >= 0x80U ) {
                const auto end = nextCodepointEnd(text, offset);
                visitor(text.substr(offset, end - offset));
                offset = end;
            } else {
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
    void placePiece(std::string_view piece, std::string_view destination,
                    float inset, float& x, float& lineY, float fontSize,
                    float lineHeight, ImU32 color, bool strong, bool code,
                    bool link)
    {
        if ( piece.empty() || m_truncated ) return;
        const bool  whitespace     = isLayoutSpace(piece.front());
        const float pieceWidth     = textWidth(piece, fontSize);
        const float availableWidth = std::max(1.0F, m_width - inset);

        if ( !whitespace && x > inset && x + pieceWidth > m_width ) {
            x = inset;
            lineY += lineHeight;
            if ( !reserveAt(lineY, lineHeight) ) return;
        }
        if ( whitespace && x <= inset + 0.01F ) return;

        if ( !whitespace && pieceWidth > availableWidth && piece.size() > 1U ) {
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
            return;
        }

        if ( code && m_drawList && !whitespace ) {
            m_drawList->AddRectFilled(
                { m_origin.x + x - 2.0F, m_origin.y + lineY - 1.0F },
                { m_origin.x + x + pieceWidth + 2.0F,
                  m_origin.y + lineY + lineHeight },
                m_style.codeBackgroundColor,
                2.0F);
        }
        if ( m_drawList && !whitespace ) {
            const ImVec2 position{ m_origin.x + x, m_origin.y + lineY };
            m_drawList->AddText(m_font,
                                fontSize,
                                position,
                                color,
                                piece.data(),
                                piece.data() + piece.size());
            if ( strong ) {
                m_drawList->AddText(m_font,
                                    fontSize,
                                    { position.x + 0.45F, position.y },
                                    color,
                                    piece.data(),
                                    piece.data() + piece.size());
            }
            if ( link ) {
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
        x += pieceWidth;
        m_maxUsedWidth = std::max(m_maxUsedWidth, x);
    }

    /// @brief 处理链接悬浮与点击。
    void handleLinkInteraction(const ImVec2& min, const ImVec2& max,
                               std::string_view destination)
    {
        if ( !m_interactiveLinks || destination.empty() ||
             !ImGui::IsWindowHovered() ||
             !ImGui::IsMouseHoveringRect(min, max, true) ) {
            return;
        }
        m_linkHovered = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if ( ImGui::IsMouseClicked(ImGuiMouseButton_Left) ) {
            DesktopPathUtils::openUrlInBrowser(destination);
        }
    }

    /// @brief 计算指定字号的文本宽度。
    float textWidth(std::string_view text, float fontSize) const
    {
        return m_font
            ->CalcTextSizeA(
                fontSize, FLT_MAX, 0.0F, text.data(), text.data() + text.size())
            .x;
    }

    /// @brief 检查当前纵坐标能否容纳指定高度。
    bool reserve(float height) { return reserveAt(m_cursorY, height); }

    /// @brief 检查指定纵坐标能否容纳指定高度。
    bool reserveAt(float y, float height)
    {
        if ( m_maxHeight > 0.0F && y + height > m_maxHeight + 0.01F ) {
            m_cursorY   = std::min(m_cursorY, m_maxHeight);
            m_truncated = true;
            return false;
        }
        return true;
    }

    /// @brief 推进纵向布局光标。
    void advance(float distance)
    {
        if ( distance <= 0.0F || m_truncated ) return;
        if ( m_maxHeight > 0.0F && m_cursorY + distance > m_maxHeight ) {
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
float resolveWrapWidth(const MarkdownRenderOptions& options)
{
    if ( options.wrapWidth > 0.0F ) return options.wrapWidth;
    return std::max(1.0F, ImGui::GetContentRegionAvail().x);
}
}  // namespace

MarkdownStyle defaultMarkdownStyle()
{
    return {
        .textColor   = ImGui::GetColorU32(ImGuiCol_Text),
        .strongColor = ImGui::GetColorU32(ImGuiCol_Text),
        .mutedColor  = ImGui::GetColorU32(ImGuiCol_TextDisabled),
        .linkColor =
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.38F, 0.68F, 1.0F, 1.0F)),
        .codeTextColor =
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.82F, 0.88F, 0.96F, 1.0F)),
        .codeBackgroundColor = ImGui::GetColorU32(ImGuiCol_FrameBg),
        .accentColor         = ImGui::GetColorU32(ImGuiCol_Border),
    };
}

MarkdownLayoutResult measureMarkdown(std::string_view             markdown,
                                     const MarkdownRenderOptions& options)
{
    const MarkdownStyle style =
        options.style ? *options.style : defaultMarkdownStyle();
    MarkdownLayout layout(nullptr,
                          { 0.0F, 0.0F },
                          resolveWrapWidth(options),
                          options.maxHeight,
                          style,
                          options.compact,
                          false);
    return layout.run(markdown);
}

void renderMarkdown(std::string_view             markdown,
                    const MarkdownRenderOptions& options)
{
    const MarkdownStyle style =
        options.style ? *options.style : defaultMarkdownStyle();
    const float    width  = resolveWrapWidth(options);
    const ImVec2   origin = ImGui::GetCursorScreenPos();
    MarkdownLayout layout(ImGui::GetWindowDrawList(),
                          origin,
                          width,
                          options.maxHeight,
                          style,
                          options.compact,
                          options.interactiveLinks);
    const auto     result = layout.run(markdown);
    ImGui::Dummy({ width, std::max(1.0F, result.size.y) });
}

MarkdownLayoutResult renderMarkdownToDrawList(
    ImDrawList& drawList, const ImVec2& min, const ImVec2& max,
    std::string_view markdown, const MarkdownRenderOptions& options)
{
    const MarkdownStyle style =
        options.style ? *options.style : defaultMarkdownStyle();
    const float rectWidth      = std::max(1.0F, max.x - min.x);
    const float rectHeight     = std::max(1.0F, max.y - min.y);
    const float width          = options.wrapWidth > 0.0F
                                     ? std::min(options.wrapWidth, rectWidth)
                                     : rectWidth;
    const float verticalOffset = std::max(
        0.0F,
        std::isfinite(options.verticalOffset) ? options.verticalOffset : 0.0F);
    const float maxHeight =
        options.maxHeight > 0.0F
            ? std::min(options.maxHeight, rectHeight) + verticalOffset
            : rectHeight + verticalOffset;
    drawList.PushClipRect(min, max, true);
    MarkdownLayout layout(&drawList,
                          { min.x, min.y - verticalOffset },
                          width,
                          maxHeight,
                          style,
                          options.compact,
                          options.interactiveLinks);
    const auto     result = layout.run(markdown);
    drawList.PopClipRect();
    return result;
}

}  // namespace MMM::UI
