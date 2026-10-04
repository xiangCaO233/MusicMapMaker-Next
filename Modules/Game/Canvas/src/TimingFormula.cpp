#include "canvas/TimingFormula.h"

#include "mmm/timing/TimingFunction.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <imgui.h>
#include <span>
#include <string_view>

namespace MMM::Canvas
{
namespace
{
/// @brief 每个节点的数学轴和包围盒，纵向组合按数学轴而非左上角对齐。
struct MathBox {
    /// @brief 公式节点总宽度。
    float m_width{};
    /// @brief 数学轴上方的空间。
    float m_above{};
    /// @brief 数学轴下方的空间。
    float m_below{};
    /// @brief 该节点字号，指数使用父节点字号的比例。
    float m_fontSize{};
};
/// @brief 文本、函数名和运算符用真实字体度量，其他结构由矢量线绘制。
/// @param text 当前有效 UTF-8 字符串视图。
/// @param size 数学字号，与节点排版使用同一单位。
/// @return 字形真实横向占位，不用字节长度估算宽度。
/// @warning 每帧度量路径，借用字体和字符串，不创建纹理或缓存文件。
/// 显式传入结束指针，不要求表达式视图后恰好有终止符。
/// 专业符号宽度与数字不同，不能用固定字符宽度拼接。
float textWidth(std::string_view text, float size)
{
    return ImGui::GetFont()
        ->CalcTextSizeA(
            size, FLT_MAX, 0, text.data(), text.data() + text.size())
        .x;
}
/// @brief 度量和绘制共用运算符，防止数学字体的宽减号被固定字符间距挤压。
/// @param kind 二元加、减或乘节点；编译树保证只传入这三类。
/// @return 正式数学运算符的 UTF-8 视图，不拥有字符串。
/// @warning 每帧布局调用；只返回静态字面量，不分配缓存。
std::string_view infixSymbol(TimingMathKind kind)
{
    return kind == TimingMathKind::Add        ? "+"
           : kind == TimingMathKind::Subtract ? "−"
                                              : "×";
}
/// @brief 同一数学树的一次帧内排版，缓存每个节点防止重复递归度量。
/// @details 数学树来自领域编译对象，本类只负责二维布局。
/// 包围盒以上方高度、下方高度和共同数学轴表示。
/// 分式、上标与聚合上下限在相同坐标约定下组合。
/// 子节点索引稳定，缓存数组容量与编译指令上限一致。
/// 树中的文本由源对象拥有，本类不复制任何字符串。
/// 根在最后一项，布局从根递归并记录每个子节点字号。
/// 大符号、分数线和根号用矢量绘制，不依赖超大字形。
/// 普通数字与名称仍采用字体度量，维持真实阅读间距。
/// 布局对象只存活于一次 render 调用，不持有跨帧所有权。
/// 源文本归一化与计算都不属于绘制职责，不能在这里再实现解析。
class FormulaLayout
{
public:
    /// @brief 借用已编译树，树中的索引不在渲染期间变化。
    explicit FormulaLayout(std::span<const TimingMathNode> nodes)
        : m_nodes(nodes)
    {
    }
    /// @brief 递归度量，每个节点只访问一次；指数和分式调整字号。
    /// @param index 已验证的根或子树索引。
    /// @param size 父节点指定的字号，指数与上下限使用比例字号。
    /// @return 该节点占用空间并同步写入帧内缓存。
    /// @pre 节点数量不超过 256，子树不存在循环。
    /// @warning 每帧执行，访问有界数学树，不进行函数求值。
    /// 嵌套缩放保留九像素下限，不能让深层上标消失。
    /// 分式的上下高度包含子表达式两侧空间及分数线留白。
    /// 乘法与加减共享水平组合规则，但各子树保持内部结构。
    /// 聚合体的第三子树正常字号排版，不与上下限一起缩小。
    /// 返回高度供子窗口扩展，不能把根式或指数裁成单行文本。
    /// 宽度只决定滚动范围，不能按窗口宽度把整条公式缩小。
    /// @note 积分列的下标不重复变量名，微分符号在主体末端表明绑定变量。
    /// @note 求和与累乘则在下标显示变量等号，上标只显示终值。
    /// @note 子树跨越多行时，下标和上标均使用完整高度留白。
    /// @note 字符度量使用显式字号，不能依赖父窗口的缩放偶然一致。
    /// @note 所有子表达式先度量后绘制，绘制函数不再修改缓存尺寸。
    /// @note 最外层公式的总高度由最高指数和最低分母共同决定。
    /// @note 对数底数宽度用小字号度量，不能按 log10 普通字符串宽度估算。
    /// @note 绝对值两条竖线围住完整子树，不与普通字符竖线混排。
    /// @note 普通函数的双参数逗号留白固定，参数自身可继续包含高分式。
    /// @note 负号节点只有左子树，不要求不存在的右参数参与度量。
    /// @note 圆括号在绘制阶段按子树高度伸展，横向仍占独立空间。
    MathBox measure(int index, float size)
    {
        const auto& node = m_nodes[index];
        size             = std::max(9.f, size);
        MathBox box{ 0, size * .5f, size * .5f, size };
        if ( node.m_kind == TimingMathKind::Text )
            box.m_width = textWidth(node.m_text, size);
        else if ( node.m_third >= 0 ) {
            // 聚合运算独立排版上下限，局部变量只出现在求和下标和积分微分中。
            // 主体维持正常字号，不能把整条积分表达式缩成上下标大小。
            const auto lower = measure(node.m_first, size * .65f);
            const auto upper = measure(node.m_second, size * .65f);
            const auto body  = measure(node.m_third, size);
            // 求和下标带变量等号，积分变量则由尾部微分表示。
            // 符号列宽覆盖最长上下限，防止界限文字伸入主体。
            // 同一列的上下限居中，主体从符号列之后开始。
            const float variable =
                node.m_kind == TimingMathKind::Integral
                    ? 0.f
                    : textWidth(node.m_text + "=", lower.m_fontSize);
            const float width = std::max(
                { size * .85f, lower.m_width + variable, upper.m_width });
            box.m_width = width + size * .25f + body.m_width;
            if ( node.m_kind == TimingMathKind::Integral )
                box.m_width += textWidth(" d" + node.m_text, size);
            box.m_above = std::max(body.m_above,
                                   size * .65f + upper.m_above + upper.m_below);
            box.m_below = std::max(body.m_below,
                                   size * .65f + lower.m_above + lower.m_below);
        } else {
            const bool fraction = node.m_kind == TimingMathKind::Fraction;
            // 分式整体稍微缩小子字号；普通组合维持正常大小。
            // 幂的第二子树单独缩小，基底高度不会跟着指数改变。
            // 所有尺寸都来自子树真实度量，不假定它们只有单个数字。
            const auto a = measure(node.m_first, size * (fraction ? .9f : 1.f));
            MathBox    b{};
            if ( node.m_second >= 0 )
                b = measure(node.m_second,
                            size * (node.m_kind == TimingMathKind::Power
                                        ? .7f
                                        : (fraction ? .9f : 1.f)));
            const float gap = size * .15f;
            // 括号槽比实际矢量曲线略宽，字符墨迹与轮廓之间另留 gap。
            // 运算符宽度必须来自字体度量，不能假定减号与数字一样宽。
            const float bracketWidth = size * .3f;
            // 分式的线在数学轴上，分子分母分别位于线上方和下方。
            // 这与简单的 a/b 文本不同，嵌套分式仍保留完整层次。
            // 数学轴就是分数线；分子最低点必须在线之上。
            // 分母最高点必须在线之下，间隔随字号变化。
            // 嵌套分式因此可以自然增加纵向高度。
            if ( fraction ) {
                box.m_width = std::max(a.m_width, b.m_width) + 2 * gap;
                box.m_above = a.m_above + a.m_below + gap;
                box.m_below = b.m_above + b.m_below + gap;
                // 上标提升位置以完整基底上界为准，而不是固定行高。
                // 底数两侧括号拥有独立占位，复杂底数不会和指数重叠。
                // 表达式 (a+b)^c 的括号需要参与宽度计算。
            } else if ( node.m_kind == TimingMathKind::Power ) {
                box.m_width =
                    a.m_width + b.m_width + 2 * bracketWidth + 2 * gap;
                box.m_above = std::max(
                    a.m_above, a.m_above + b.m_above + b.m_below - size * .15f);
                box.m_below = a.m_below;
                // 根号横线覆盖被开方表达式全宽，顶部再保留少量间隔。
                // 根式的下面高度仍由主体决定，不把根号当成固定字符。
                // 三次根的左上索引占用同一前缀区域。
            } else if ( node.m_kind == TimingMathKind::Root ||
                        node.m_kind == TimingMathKind::CubeRoot ) {
                box.m_width = a.m_width + size * .8f;
                box.m_above = a.m_above + gap;
                box.m_below = a.m_below;
            } else {
                // 横排组合采用独立运算符宽度；括号始终保留运算分组。
                box.m_above = std::max(a.m_above, b.m_above);
                box.m_below = std::max(a.m_below, b.m_below);
                // 函数名后保留括号空间，双参数函数另给逗号留白。
                // 对数底数采用下标宽度，不按 log10 的五个普通字符估算。
                // 左右括号可按整体高度伸展，不依赖字体括号原始行高。
                // 对数底数独立采用小字号下移，其余函数名保持普通数学轴。
                // 函数参数本身可能含分式，括号需随完整包围高度伸展。
                // 双参数函数用逗号分隔，不能误画成两个邻接的乘数。
                if ( node.m_kind == TimingMathKind::Function )
                    box.m_width =
                        (node.m_text == "log10" || node.m_text == "log2"
                             ? textWidth("log", size) +
                                   textWidth(node.m_text.substr(3), size * .65f)
                             : textWidth(node.m_text, size)) +
                        2 * bracketWidth + 2 * gap + a.m_width +
                        (node.m_second >= 0
                             ? b.m_width + textWidth(",", size) + 2 * gap
                             : 0);
                else if ( node.m_kind == TimingMathKind::Absolute )
                    box.m_width = a.m_width + 2 * gap;
                else if ( node.m_kind == TimingMathKind::Negate )
                    // 一元负号、两侧括号及子表达式各有独立占位。
                    // 不能把“−(”当普通文本后只前移固定半个字号。
                    box.m_width = a.m_width + textWidth("−", size) +
                                  2 * bracketWidth + 3 * gap;
                else
                    box.m_width = a.m_width + b.m_width +
                                  textWidth(infixSymbol(node.m_kind), size) +
                                  2 * bracketWidth + 4 * gap;
            }
        }
        m_boxes[index] = box;
        return box;
    }
    /// @brief 随子表达式高度伸展的矢量括号，不依赖字体提供可伸缩字形。
    /// @param x 括号占位的左边界。
    /// @param y 共同数学轴。
    /// @param box 被包围表达式的纵向包围。
    /// @param closing 真表示右括号，假表示左括号。
    /// @warning 每帧排版路径，只提交有界曲线几何。
    /// @details 括号的曲率只取决于包围高度，不执行数学函数。
    /// 两个控制点分别使用上部和下部距离，适应不对称分式。
    /// 左右括号关于自身占位镜像，闭合括号不覆盖主体末字符。
    /// 厚度固定为可读轮廓，尺寸变化交给 ImGui 的像素缩放。
    /// 曲线自身没有填充，主体内容不会被括号背景遮住。
    /// 此结构用于幂基底及函数参数，不重新决定运算优先级。
    /// 包围可以来自嵌套积分，上下限高度仍参与括号延伸。
    /// 所有控制点为有限投影值，编译器不会提供循环节点树。
    /// 只使用当前窗口 DrawList，子窗口的剪裁范围自然生效。
    /// 几何以浮点存储，缩放不先取整导致短括号断裂。
    void bracket(float x, float y, const MathBox& box, bool closing) const
    {
        const float width = box.m_fontSize * .25f;
        const float edge  = closing ? x : x + width;
        const float bulge = closing ? x + width : x;
        ImGui::GetWindowDrawList()->AddBezierCubic(
            ImVec2(edge, y - box.m_above),
            ImVec2(bulge, y - box.m_above * .6f),
            ImVec2(bulge, y + box.m_below * .6f),
            ImVec2(edge, y + box.m_below),
            IM_COL32(215, 235, 245, 255),
            1.3f);
    }
    /// @brief 按度量结果绘制节点，坐标 y 表示数学轴。
    /// @warning 每帧公式绘制；只遍历最多 256 个缓存节点，不解析或执行函数。
    /// @param index 已经调用 measure 的节点索引。
    /// @param x 当前表达式的屏幕左边界。
    /// @param y 当前表达式的共同数学轴。
    /// @pre 同一次布局中必须先完成完整树度量。
    /// 分子、分母和上标的 y 偏移均由缓存包围盒计算。
    /// 文字入口显式使用节点字号，不能复用全局字号造成错位。
    /// 颜色统一是公式前景色，不改变用户可编辑的输入文本。
    /// 树的第三子节点只属于聚合，不能按二元乘法兜底绘制。
    /// 所有结构的实际横向增量与 measure 返回的占位保持一致。
    /// 大符号不使用位图缩放，窗口放大只增加矢量几何尺寸。
    /// 方法不修改模型缓存，也不会触发资源加载或数学求值。
    void draw(int index, float x, float y) const
    {
        const auto& node = m_nodes[index];
        const auto& box  = m_boxes[index];
        const float size = box.m_fontSize, gap = size * .15f;
        // 与 measure 共用括号槽和留白；实际子树宽度来自本帧缓存。
        const float bracketWidth = size * .3f;
        // 字体文本以数学轴为中心，字号与基准行距独立。
        // 视图的剪裁由 ImGui 子窗口维护，超宽公式通过滚动阅读。
        // 字符串视图的末指针显式传入，防止读取节点之外的字符。
        const auto text =
            [&](std::string_view value, float tx, float ty, float fontSize) {
                ImGui::GetWindowDrawList()->AddText(
                    ImGui::GetFont(),
                    fontSize,
                    ImVec2(tx, ty - fontSize * .5f),
                    IM_COL32(215, 235, 245, 255),
                    value.data(),
                    value.data() + value.size());
            };
        auto*           list  = ImGui::GetWindowDrawList();
        constexpr ImU32 color = IM_COL32(215, 235, 245, 255);
        if ( node.m_kind == TimingMathKind::Text ) {
            text(node.m_text, x, y, size);
            return;
        }
        const auto&   a = m_boxes[node.m_first];
        const MathBox b =
            node.m_second >= 0 ? m_boxes[node.m_second] : MathBox{};
        if ( node.m_third >= 0 ) {
            const auto& body     = m_boxes[node.m_third];
            const bool  integral = node.m_kind == TimingMathKind::Integral;
            const float variable =
                integral ? 0.f : textWidth(node.m_text + "=", a.m_fontSize);
            const float width =
                std::max({ size * .85f, a.m_width + variable, b.m_width });
            const float center = x + width * .5f;
            // 聚合大符号以矢量绘制，避免运行字体缺少大号积分或累乘字形。
            // 上下限从符号两端向外展开，嵌套分式不会与符号相互覆盖。
            const float left  = center - size * .32f,
                        right = center + size * .32f;
            const float top = y - size * .55f, bottom = y + size * .55f;
            if ( integral ) {
                list->AddBezierCubic(ImVec2(right, top),
                                     ImVec2(left, top - size * .3f),
                                     ImVec2(right, bottom + size * .3f),
                                     ImVec2(left, bottom),
                                     color,
                                     1.8f);
            } else if ( node.m_kind == TimingMathKind::Sum ) {
                list->AddLine(
                    ImVec2(right, top), ImVec2(left, top), color, 1.8f);
                list->AddLine(ImVec2(left, top),
                              ImVec2(center + size * .1f, y),
                              color,
                              1.8f);
                list->AddLine(ImVec2(center + size * .1f, y),
                              ImVec2(left, bottom),
                              color,
                              1.8f);
                list->AddLine(
                    ImVec2(left, bottom), ImVec2(right, bottom), color, 1.8f);
            } else {
                list->AddLine(
                    ImVec2(left, top), ImVec2(right, top), color, 1.8f);
                list->AddLine(ImVec2(left + size * .08f, top),
                              ImVec2(left + size * .08f, bottom),
                              color,
                              1.8f);
                list->AddLine(ImVec2(right - size * .08f, top),
                              ImVec2(right - size * .08f, bottom),
                              color,
                              1.8f);
            }
            // 上下限分别按自己的完整宽度居中，不能把字节数作为宽度。
            // 下标 y 从符号底部加子树上界，复杂分式界限也不会碰到符号。
            // 上标 y 从符号顶部减子树下界，界限可包含另一个幂。
            const float lowerX = x + (width - a.m_width - variable) * .5f;
            const float lowerY = y + size * .65f + a.m_above;
            if ( !integral )
                text(node.m_text + "=", lowerX, lowerY, a.m_fontSize);
            draw(node.m_first, lowerX + variable, lowerY);
            draw(node.m_second,
                 x + (width - b.m_width) * .5f,
                 y - size * .65f - b.m_below);
            draw(node.m_third, x + width + size * .25f, y);
            if ( integral )
                text(" d" + node.m_text,
                     x + width + size * .25f + body.m_width,
                     y,
                     size);
            return;
        }
        // 分子和分母水平居中，允许两侧长度完全不同。
        // 分数线覆盖当前盒宽，与被除式文字的基线没有关联。
        // 绘制后立即返回，避免该节点再进入普通运算符分支。
        if ( node.m_kind == TimingMathKind::Fraction ) {
            draw(node.m_first,
                 x + (box.m_width - a.m_width) / 2,
                 y - gap - a.m_below);
            draw(node.m_second,
                 x + (box.m_width - b.m_width) / 2,
                 y + gap + b.m_above);
            list->AddLine(
                ImVec2(x, y), ImVec2(x + box.m_width, y), color, 1.3f);
            return;
        }
        if ( node.m_kind == TimingMathKind::Power ) {
            // 底数使用括号保持 (a+b)^c 的真实语义；指数提升到基底上方。
            bracket(x, y, a, false);
            draw(node.m_first, x + bracketWidth + gap, y);
            bracket(x + bracketWidth + 2 * gap + a.m_width, y, a, true);
            draw(node.m_second,
                 x + a.m_width + 2 * bracketWidth + 2 * gap,
                 y - a.m_above - b.m_below + size * .15f);
            return;
        }
        if ( node.m_kind == TimingMathKind::Root ||
             node.m_kind == TimingMathKind::CubeRoot ) {
            // 根号使用矢量折线和覆盖横线，长度随被开方表达式自然扩展。
            // 不依赖某个字体恰好有大尺寸根号；三次根的指数单独排版。
            const float start = x + size * .2f, body = x + size * .7f,
                        top = y - a.m_above - gap;
            list->AddLine(ImVec2(start, y),
                          ImVec2(start + size * .15f, y + a.m_below * .7f),
                          color,
                          1.3f);
            list->AddLine(ImVec2(start + size * .15f, y + a.m_below * .7f),
                          ImVec2(body - size * .08f, top),
                          color,
                          1.3f);
            list->AddLine(ImVec2(body - size * .08f, top),
                          ImVec2(x + box.m_width, top),
                          color,
                          1.3f);
            if ( node.m_kind == TimingMathKind::CubeRoot )
                text("3", x, y - size * .5f, size * .6f);
            draw(node.m_first, body, y);
            return;
        }
        // 绝对值竖线随内部高度延伸，不能用普通字符截断高分式。
        // 两侧间隔都来自字号，主体位于竖线内部而非外部。
        // 绝对值没有右子树，与函数双参数布局区分处理。
        if ( node.m_kind == TimingMathKind::Absolute ) {
            list->AddLine(ImVec2(x, y - a.m_above),
                          ImVec2(x, y + a.m_below),
                          color,
                          1.3f);
            list->AddLine(ImVec2(x + box.m_width, y - a.m_above),
                          ImVec2(x + box.m_width, y + a.m_below),
                          color,
                          1.3f);
            draw(node.m_first, x + gap, y);
            return;
        }
        // 负号使用真正的数学减号，复杂负值被明确括起来。
        // 负号自身不改变主体字号，主体仍使用原来的数学轴。
        // 幂优先级已经由编译树决定，渲染不重新猜测运算顺序。
        if ( node.m_kind == TimingMathKind::Negate ) {
            text("−", x, y, size);
            // 负号与括号分开绘制，避免数学字体回退改变“−(”整体宽度。
            // 括号按完整子树高度伸展，分式不会穿过小字号文本括号。
            const float opening = x + textWidth("−", size) + gap;
            bracket(opening, y, a, false);
            draw(node.m_first, opening + bracketWidth + gap, y);
            bracket(x + box.m_width - bracketWidth, y, a, true);
            return;
        }
        if ( node.m_kind == TimingMathKind::Function ) {
            if ( node.m_text == "log10" || node.m_text == "log2" ) {
                text("log", x, y, size);
                x += textWidth("log", size);
                text(node.m_text.substr(3), x, y + size * .3f, size * .65f);
                x += textWidth(node.m_text.substr(3), size * .65f);
            } else {
                text(node.m_text, x, y, size);
                x += textWidth(node.m_text, size);
            }
            bracket(x, y, box, false);
            x += bracketWidth + gap;
            draw(node.m_first, x, y);
            x += a.m_width;
            if ( node.m_second >= 0 ) {
                x += gap;
                text(",", x, y, size);
                x += textWidth(",", size) + gap;
                draw(node.m_second, x, y);
                x += b.m_width;
            }
            bracket(x + gap, y, box, true);
            return;
        }
        // 加减乘采用正式 Unicode 数学运算符，不能用连字符混淆负号。
        const auto symbol = infixSymbol(node.m_kind);
        bracket(x, y, box, false);
        const float first     = x + bracketWidth + gap;
        const float operatorX = first + a.m_width + gap;
        // 二元算符的左右均留白，嵌套乘法不能盖住相邻负数首字符。
        // 实际绘制步进与度量宽度一致，右括号不得落入最后一个数字。
        draw(node.m_first, first, y);
        text(symbol, operatorX, y, size);
        draw(node.m_second, operatorX + textWidth(symbol, size) + gap, y);
        bracket(x + box.m_width - bracketWidth, y, box, true);
    }

private:
    /// @brief 借用程序拥有的公式树，避免每帧复制字符串和共享所有权。
    std::span<const TimingMathNode> m_nodes;
    /// @brief 上限与编译器指令预算一致，大小不由用户文本控制。
    std::array<MathBox, 256> m_boxes{};
};
}  // namespace
/// @brief 用可水平滚动的公式区域保持字号，不把长函数缩成模糊小字。
/// @warning 每帧编辑预览；节点树来自编译缓存，没有表达式重解析。
/// @param function 由窗口副本持续持有的不可变编译对象。
/// @pre 调用方已在当前 ImGui 窗口内。
/// 空树或超预算树不提交绘制，防止非法索引进入布局器。
/// 前缀 f(t) 与主体共用数学轴，主公式的高度决定整个区域。
/// 公式默认字号比正文大四分之一，横向空间不足时启用滚动。
/// 高度包括最外层的上下限与指数，不固定成输入文本行高。
/// 子窗口 ID 固定，表达式文字变化不创建新的滚动窗口。
/// Dummy 仅报告内容边界，不把公式转换成不可编辑图片。
/// 当前布局对象在函数返回时销毁，源节点仍由编译对象拥有。
void renderTimingFormula(const TimingFunction& function)
{
    const auto nodes = timingFunctionMathNodes(function);
    if ( nodes.empty() || nodes.size() > 256 ) return;
    FormulaLayout layout(nodes);
    const float   fontSize = ImGui::GetFontSize() * 1.25f;
    const auto    box =
        layout.measure(static_cast<int>(nodes.size() - 1), fontSize);
    // 内容宽度只影响横向滚动；纵向必须容纳完整公式、内边距和横向滚动条。
    // 不能只按公式高度加固定像素，高 DPI 样式中的滚动条会挤出纵向滚动。
    const auto&  style  = ImGui::GetStyle();
    const float  prefix = textWidth("f(t) = ", fontSize);
    const ImVec2 contentSize(prefix + box.m_width + 8,
                             box.m_above + box.m_below + 8);
    const float  height =
        std::max(70.f,
                 contentSize.y + 2 * style.WindowPadding.y +
                     2 * style.ChildBorderSize + style.ScrollbarSize);
    // 在 BeginChild 前发布本帧完整尺寸，函数和字号变化不沿用上帧滚动范围。
    // 保留横向位置，纵向始终回到完整可见的数学包围框。
    ImGui::SetNextWindowContentSize(contentSize);
    ImGui::SetNextWindowScroll(ImVec2(-1, 0));
    ImGui::BeginChild("公式排版",
                      ImVec2(0, height),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  axis   = origin.y + box.m_above + 4;
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),
                                        fontSize,
                                        ImVec2(origin.x, axis - fontSize * .5f),
                                        IM_COL32(215, 235, 245, 255),
                                        "f(t) = ");
    layout.draw(static_cast<int>(nodes.size() - 1), origin.x + prefix, axis);
    ImGui::Dummy(contentSize);
    ImGui::EndChild();
}
}  // namespace MMM::Canvas
