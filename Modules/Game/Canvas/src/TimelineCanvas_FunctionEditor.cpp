#include "canvas/TimelineCanvas.h"
#include "mmm/timing/TimingInterpolationAlgorithms.h"
#include "presentation/TimelineInteractionPresentation.h"
#include "ui/interaction/EditorInteraction.h"

#include "canvas/TimingFormula.h"
#include "canvas/TimingInterpolationPreview.h"
#include "config/AppConfig.h"
#include "config/skin/translation/Translation.h"
#include "mmm/timing/TimingFunction.h"
#include "mmm/timing/TimingFunctionFit.h"
#include "ui/utils/UIWidgetUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <imgui.h>
#include <string>
#include <string_view>
#include <utility>

namespace MMM::Canvas
{
namespace
{
/// @brief 旧数学结果保留稳定值语义，UI 边界只翻译已知固定提示与函数族。
/// @details 未识别的解析错误仍完整显示原文，不把用户输入当作翻译键。
/// @warning 参数帧仅比较有限短表，不分配字符串或重新编译表达式。
const char* fitDisplayText(const std::string& value)
{
    // 模型不是 UI 模块，不引入翻译依赖，也不改变已持久化公式源码。
    // 返回值借用模型字符串或翻译缓存，只在本次绘制期间使用。
    constexpr std::array pairs{
        std::pair{ "多项式复合", "ui.timing.curve.family_polynomial" },
        std::pair{ "正弦与余弦复合", "ui.timing.curve.family_fourier" },
        std::pair{ "指数复合", "ui.timing.curve.family_exponential" },
        std::pair{ "对数复合", "ui.timing.curve.family_logarithm" },
        std::pair{ "平方根复合", "ui.timing.curve.family_sqrt" },
        std::pair{ "立方根复合", "ui.timing.curve.family_cbrt" },
        std::pair{ "拟合结果已失效，请重新拟合。",
                   "ui.timing.curve.fit_stale" },
        std::pair{ "拟合函数超出当前段落的合法参数范围，请调整后重新拟合。",
                   "ui.timing.curve.fit_invalid" }
    };
    for ( const auto& [source, key] : pairs )
        if ( value == source ) return TR(key).data();
    // 未知动态解析错误原样保留，不能丢失具体位置或用户输入片段。
    return value.c_str();
}
/// @brief 安全复制到固定编辑缓冲，超出预算时不截断成另一条函数。
/// @param state 当前模态窗口拥有的缓冲。
/// @param source 完整 UTF-8 表达式，字节数须小于固定容量。
/// @return 完整复制时为真，容量不足时保留原文本。
/// 复制使用字节容量而不是字形数量，防止切断多字节数学符号。
/// 终止符单独写入，InputText 不会访问旧表达式残留的尾部。
/// 此函数用于窗口初始化和应用候选，不在每帧复制整条公式。
bool copyExpression(TimingFunctionEditorState& state, std::string_view source)
{
    if ( source.size() >= state.m_expression.size() ) return false;
    std::copy(source.begin(), source.end(), state.m_expression.begin());
    state.m_expression[source.size()] = '\0';
    return true;
}
/// @brief 数学符号的输入模板，保留 Unicode 而非强迫用户查 ASCII 名称。
struct MathInput {
    /// @brief 按钮显示正式数学符号或函数名。
    const char* m_label;
    /// @brief 可直接插入的完整表达式模板，用户随后调整参数。
    const char* m_input;
    /// @brief 悬浮说明参数顺序、变量单位和一个可直接输入的例子。
    const char* m_help;
    /// @brief 需要正式上下标排版时的基底，普通符号为 nullptr。
    const char* m_base{};
    /// @brief 独立排版的小字号指数或底数。
    const char* m_script{};
    /// @brief 真表示上标，假表示下标。
    bool m_superscript{};
};
/// @brief 常用符号与初等函数入口，模板不包含可执行脚本。
/// @details 点击插入的是计算器支持的语法，不把界面标签视为可执行名称。
/// 函数名与参数顺序直接展示在 Tooltip 内，用户不必猜测输入方式。
/// 模板采用完整参数和右括号，插入之后可直接修改。
/// 幂的上标属于后缀操作，必须跟在已有底数后面。
/// 根号模板使用正式符号，编译器将其归一为同一数学操作。
/// 对数底数标签是专业表示，输入分别使用 log10 与 log2。
/// 自然对数统一使用 ln，log 别名由计算器接收。
/// 积分的局部变量与 t 独立，提示明确说明 t 仍是段内秒数。
/// Σ 与 Π 的模板使用有限整数上下限，不隐含无限级数。
/// 大符号本身只用于按钮显示，完整函数式由输入模板给出。
/// 提示文本也说明定义域和工作量上限，而非只列函数名称。
/// 按钮反馈仍经过统一入口，数学面板不创建特殊声音状态。
/// 数学符号采用 UTF-8 文本，禁止按字符数截断缓冲。
/// 每个模板的例子与模型测试中独立的数学答案一致。
/// 正式排版来自编译树，面板不是另一套公式解析器。
constexpr std::array SYMBOLS{
    MathInput{ "π", "π", "ui.timing.curve.symbol_0" },
    MathInput{ "e", "e", "ui.timing.curve.symbol_1" },
    MathInput{ "×", "×", "ui.timing.curve.symbol_2" },
    MathInput{ "÷", "÷", "ui.timing.curve.symbol_3" },
    MathInput{ "−", "−", "ui.timing.curve.symbol_4" },
    MathInput{ "x²", "²", "ui.timing.curve.symbol_5" },
    MathInput{ "x³", "³", "ui.timing.curve.symbol_6" },
    MathInput{ "xⁿ", "^(2)", "ui.timing.curve.symbol_7", "x", "n", true },
    MathInput{ "√", "√(t)", "ui.timing.curve.symbol_8" },
    MathInput{ "∛", "∛(t)", "ui.timing.curve.symbol_9" },
    MathInput{ "ln", "ln(1+t)", "ui.timing.curve.symbol_10" },
    MathInput{ "log₁₀",
               "log10(1+t)",
               "ui.timing.curve.symbol_11",
               "log",
               "10",
               false },
    MathInput{
        "log₂", "log2(1+t)", "ui.timing.curve.symbol_12", "log", "2", false },
    MathInput{ "sin", "sin(t)", "ui.timing.curve.symbol_13" },
    MathInput{ "cos", "cos(t)", "ui.timing.curve.symbol_14" },
    MathInput{ "tan", "tan(t/4)", "ui.timing.curve.symbol_15" },
    MathInput{ "exp", "exp(t)", "ui.timing.curve.symbol_16" },
    MathInput{ "sinh", "sinh(t)", "ui.timing.curve.symbol_17" },
    MathInput{ "cosh", "cosh(t)", "ui.timing.curve.symbol_18" },
    MathInput{ "tanh", "tanh(t)", "ui.timing.curve.symbol_19" },
    MathInput{ "|x|", "abs(t)", "ui.timing.curve.symbol_20" },
    MathInput{ "∫", "int(x,0,t,x²)", "ui.timing.curve.symbol_21" },
    MathInput{ "Σ", "sum(k,1,10,k²)", "ui.timing.curve.symbol_22" },
    MathInput{ "Π", "prod(k,1,5,k)", "ui.timing.curve.symbol_23" }
};
/// @brief 数学标签使用独立上下标排版，避免字体缺少 Unicode 上下标字形。
/// @note 符号的可见排版与输入函数式共用同一条模板身份。
/// @param symbol 当前按钮的符号、模板和可选数学结构。
/// @return 完整反馈按钮被用户点击时返回真。
/// @warning 每帧面板绘制，仅度量固定短标签，不构造动态字符串。
/// @note 普通符号继续用统一小按钮，上下标按钮同样经过 FeedbackButton。
/// 节点字号随正文变化，底数不是普通字符挤在 log 后面。
/// ID 使用稳定模板，文字绘制不改变按钮的悬浮命中范围。
/// @details 上下标的字符使用普通数字或字母，不依赖 Unicode 专属小字形。
/// 按钮的 ID 与可见标签分离，翻译和字体回退不改变交互身份。
/// 字号、内边距和文字颜色全部取自当前窗口样式。
/// 基底宽度使用实际字体度量，禁止以 UTF-8 字节数估算。
/// 小字位置留在按钮高度内，不能覆盖下方一行符号。
/// 右上标用于幂，右下标用于对数底数；两者不能混用。
/// 自定义文字只是绘制，不另建命中区或抢占鼠标焦点。
/// 反馈按钮返回值仍来自标准按下/释放手势。
/// 绘制后紧邻的 IsItemHovered 继续查询该同一个按钮。
/// 函数式模板与标签绑定，界面显示不能修改实际插入语法。
/// @param touchHeight 零保持桌面小按钮，正值为移动端最小触摸高度。
/// @note 只改变排版与热区，符号模板和 InputText 插入规则保持不变。
bool mathSymbolButton(const MathInput& symbol, float touchHeight)
{
    if ( !symbol.m_base )
        return touchHeight > 0
                   ? UI::FeedbackButton(
                         symbol.m_label,
                         { std::max(touchHeight,
                                    ImGui::CalcTextSize(symbol.m_label).x +
                                        2 * ImGui::GetStyle().FramePadding.x),
                           touchHeight })
                   : UI::FeedbackSmallButton(symbol.m_label);
    const auto size      = ImGui::GetFontSize();
    const auto small     = size * .65f;
    const auto baseWidth = ImGui::CalcTextSize(symbol.m_base).x;
    const auto scriptWidth =
        ImGui::GetFont()->CalcTextSizeA(small, 1000, 0, symbol.m_script).x;
    const auto padding = ImGui::GetStyle().FramePadding;
    // 隐藏文本的按钮仍拥有完整反馈外观，字形按真实尺寸放在其内部。
    ImGui::PushID(symbol.m_input);
    const bool clicked = UI::FeedbackButton(
        "##mathSymbol",
        ImVec2(std::max(touchHeight, baseWidth + scriptWidth + 2 * padding.x),
               std::max(touchHeight, size + 2 * padding.y)));
    ImGui::PopID();
    const auto origin = ImGui::GetItemRectMin();
    const auto color  = ImGui::GetColorU32(ImGuiCol_Text);
    auto*      draw   = ImGui::GetWindowDrawList();
    draw->AddText(ImGui::GetFont(),
                  size,
                  ImVec2(origin.x + padding.x, origin.y + padding.y),
                  color,
                  symbol.m_base);
    // 小字号分别提升或下移，保持幂与对数底数的专业数学含义。
    draw->AddText(
        ImGui::GetFont(),
        small,
        ImVec2(origin.x + padding.x + baseWidth,
               origin.y + padding.y + (symbol.m_superscript ? 0 : size * .4f)),
        color,
        symbol.m_script);
    return clicked;
}
/// @brief 用 InputText 正式回调插入模板，保留真实光标和选择替换语义。
/// @param data ImGui 当前输入框的可编辑回调数据。
/// @return 不过滤输入，模板处理完成后始终返回零。
/// @pre UserData 指向当前模态窗口的编辑状态。
/// @warning 活动输入框每帧调用；没有待插入内容时立即返回。
/// 选区和光标偏移均为 UTF-8 字节位置，由 ImGui 维护边界。
/// 不通过私有 InputTextState 改写文本，避免绑定内部版本布局。
/// 替换前预检剩余容量，失败不会删除已选中的文字。
/// DeleteChars 后显式把光标放到选区起点，避免反向选择的偏移歧义。
/// 插入使用正式 InsertChars，输入框的脏标记和光标同步由 ImGui 更新。
/// 待插入模板只消费一次，不因后续 CallbackAlways 重复写入。
/// 状态指针只在 InputText 调用期间借用，不注册跨帧回调对象。
/// 队列字符串用于按钮交互，普通帧不会构造新的模板副本。
int mathInputCallback(ImGuiInputTextCallbackData* data)
{
    auto& state = *static_cast<TimingFunctionEditorState*>(data->UserData);
    if ( state.m_pendingInsertion.empty() ) return 0;
    const int begin    = std::min(data->SelectionStart, data->SelectionEnd);
    const int selected = std::abs(data->SelectionStart - data->SelectionEnd);
    // 先确认替换后的 UTF-8 文本容量，不能截断数学符号或破坏旧输入。
    if ( static_cast<std::size_t>(data->BufTextLen - selected) +
             state.m_pendingInsertion.size() <
         static_cast<std::size_t>(data->BufSize) ) {
        if ( selected ) {
            data->DeleteChars(begin, selected);
            data->CursorPos = begin;
        }
        data->InsertChars(data->CursorPos, state.m_pendingInsertion.c_str());
    }
    state.m_pendingInsertion.clear();
    return 0;
}
/// @brief 手绘点使用实际参数值，坐标轴调整不改变已经绘制的含义。
/// @param origin 绘制区域屏幕坐标左上角。
/// @param size 当前可见区域像素尺寸。
/// @param x 归一横轴，不改变模型中 t 的秒单位。
/// @param value 实际 Timing 参数，不是 0–1 插值比例。
/// @param minimum 纵轴的有限下界。
/// @param maximum 严格大于下界的有限上界。
/// @return 参数在屏幕上的投影，可超出视图而由统一剪裁处理。
/// @pre 调用方先验证纵轴跨度和段落时长。
/// @warning 绘图每帧使用，只有常量标量计算。
/// 纵轴方向反转是屏幕坐标要求，不意味着参数或曲线取反。
ImVec2 curvePoint(const ImVec2& origin, const ImVec2& size, double x,
                  double value, double minimum, double maximum)
{
    return ImVec2(
        origin.x + static_cast<float>(x) * size.x,
        origin.y + static_cast<float>((maximum - value) / (maximum - minimum)) *
                       size.y);
}
}  // namespace

/// @brief 为新一次模态编辑初始化表达式及绘制状态。
/// @warning 用户打开窗口时执行；不在每帧或会话播放更新中重置。
/// @details 初始化只消费模态工作副本，不写活动谱面。
/// 旧表达式优先复制源文本，避免把原函数重新近似成预设。
/// 普通预设切到自定义时提供显式线性起终值模板。
/// 贝塞尔保留在预设模式，线性模板不冒充已有曲率。
/// 绘制纵轴以完整函数包围初始化，内部峰谷不能被端值遗漏。
/// 空常值范围同样给出非零边距，保证鼠标投影有有效分母。
/// 编译时长设为无效值，使首次自定义帧执行一次编译。
/// 每次打开都清空绘制与拟合状态，避免另一段落的候选泄漏。
/// 当前对象拥有全部 UI 状态，关闭窗口不需要跨线程回收。
void TimelineCanvas::initializeTimingFunctionEditor()
{
    // 每次载入新的工作副本清除旧图表归属，避免同路径重开后继续旧笔画。
    m_timingPresentation->resetCurveInput();
    // 函数定义域取当前自变量单位，秒数只负责定位段落。
    // 拍域手绘格点在拍数上均分，而非在真实秒数上均分。
    // 分拍不改变 t 的拍单位，也不改变拟合的数学形状。
    // 修改范围后候选须按新域校验，不能沿用旧拍长。
    // 蓝线与手绘共享自变量域，输出图再映射回时间域。
    // 源式或域变化才编译，普通帧不遍历红线时间线。
    // f(t) 记号不变，横轴和说明明确秒或拍单位。
    auto& state       = m_timingFunctionEditor;
    state             = {};
    const auto& edit  = m_interpolationEdit;
    const auto& curve = edit.interpolation;
    if ( curve.m_curve == TimingCurve::Custom && curve.m_function )
        copyExpression(state, timingFunctionExpression(*curve.m_function));
    else {
        // 首次切到自由函数时提供可编辑的秒域起终值表达式。
        // 贝塞尔的横轴反解仍由预设模式处理，不声称线性模板等价于贝塞尔。
        copyExpression(state,
                       fmt::format("{:.17g}+({:.17g})*t/{:.17g}",
                                   edit.value,
                                   curve.m_endValue - edit.value,
                                   timingInterpolationVariableDuration(curve)));
    }
    const auto [minimum, maximum] = timingInterpolationRange(curve, edit.value);
    const double padding =
        std::max({ (maximum - minimum) * .2, std::abs(minimum) * .1, 1.0 });
    state.m_minimum = minimum - padding;
    state.m_maximum = maximum + padding;
    // 默认 BPM 纵轴保持为正，用户仍可显式调整，最终保存使用完整域校验。
    if ( edit.effect == TimingEffect::BPM )
        state.m_minimum = std::max(.1, state.m_minimum);
    state.m_compiledDuration = -1;
}

/// @brief 展示可编辑公式、正式排版及覆盖整段的手绘拟合工具。
/// @warning 每帧编辑窗口路径；仅文本变化、时长变化和拟合按钮触发编译或求解。
/// @details 文本编译与拟合共用窗口副本，输出预览始终读取当前有效函数。
/// 文本改变只替换窗口副本，最终保存才形成逻辑命令。
/// 段落时长变化必须重新编译，不能继续使用旧定义域缓存。
/// 编辑失败保留文本；旧有效对象不能令保存入口误判新文本有效。
/// 数学排版借用已编译树，禁止每帧从字符串重新解析。
/// 绘制按固定格点即时更新，不等待消抖计时器或工作线程。
/// 拟合按钮属于低频计算，普通拖动不会在每帧求解 QR。
/// 尚未绘制的格点不会用参考曲线自动补齐，以免伪造用户曲线。
/// 成功拟合即时更新窗口副本，同时展示误差供保存前检查。
/// 候选的完整定义域还须通过当前 Timing 效果限制。
/// BPM 为正不能只检查起终值，拟合过冲也应拒绝。
/// 绘制纵轴只影响投影，不修改已经画出的参数值。
/// 改变时长会使秒域候选失效，原归一格点保留供重新拟合。
/// 候选应用与最终保存分开，关闭窗口仍能放弃全部编辑。
/// 共享函数副本只在拟合成功或显式重新采用时转交所有权。
/// 原函数图及输出密度由外层窗口维护，不由该面板生成 Timing 事件。
/// 窗口跨帧保持固定缓冲，回调不捕获临时字符串。
/// 所有动态文字作为参数传给 ImGui，不能成为格式串。
/// 完整表达式上限与编译器一致，面板不接受截断后的公式。
/// @note 符号列表列数依可用宽度调整，窗口缩窄也不能产生不可访问按钮。
/// @note Tooltip 只读取固定模板，不触发拟合、资源查询或代码执行。
/// @note 已选择文本的替换由输入回调完成，不能在按钮点击处直接改缓冲。
/// @note 模板超容量时保留旧表达式和选区，不截断成另一条数学含义。
/// @note 原曲线与候选使用不同子窗口 ID，横向滚动不会相互串扰。
/// @note 改变效果类型后重新检查候选范围，不能保留上一效果的禁用状态。
/// @note 区间内误差用真实参数单位表示，不把画布像素误差当成数值误差。
/// @note 黄色和蓝色绘图共享相同坐标轴，比较不能因视图变化伪造改善。
/// @note 未画满的手势只能显示，求解不默默补充用户没有给出的数据。
/// @note 应用候选后源文本、起值、终值和时长标记同时更新。
/// @note 开始新一次绘制即使拟合失败，也必须使上一候选不可再应用。
/// @note 轴输入支持分步修改，无效中间值显示提示但不自动钳制用户文本。
/// @note 拟合错误与表达式编译错误分别保存，不能由另一个面板的成功清空。
/// @note 显示候选并不意味谱面已更新，逻辑会话仍只消费最终确认命令。
/// @note 手绘函数家族搜索有明确列数与点数上限，不执行自由递归优化。
/// @note UI 状态只在所属画布存活，不将 ImGui 指针发送到逻辑线程。
/// @note 拟合源始终可重新编辑；保存不依赖手绘格点继续存在。
/// @note 预览图通过读取不可变函数求值，不维护另一条与公式脱节的曲线。
void TimelineCanvas::renderTimingFunctionEditor()
{
    auto& edit  = m_interpolationEdit;
    auto& curve = edit.interpolation;
    auto& state = m_timingFunctionEditor;
    if ( state.m_fitFunction &&
         timingFunctionDuration(*state.m_fitFunction) !=
             timingInterpolationVariableDuration(curve) ) {
        // 绘制点按比例保留，但旧拟合属于旧秒域，范围修改后立即丢弃。
        state.m_fit.reset();
        state.m_fitFunction.reset();
        state.m_fitError.clear();
    }
    if ( curve.m_curve == TimingCurve::Custom ) {
        bool changed = false;
        // 面板可折叠，使用符号与语法之间的明确映射。
        // 悬浮说明由紧邻的按钮决定，不依赖当前选中的函数模式。
        // Tooltip 不转移输入焦点，用户仍可直接编辑原公式。
        if ( ImGui::TreeNode(TR("ui.timing.curve.symbols").data()) ) {
            const float touchHeight =
                m_timingPresentation->parameterButtonHeight(
                    Config::AppConfig::instance().getWindowContentScale());
            // 根据最宽短标签选择列数，缩窄窗口也不会把符号按钮画出右边界。
            float widest = 0;
            for ( const auto& symbol : SYMBOLS ) {
                const auto width =
                    ImGui::CalcTextSize(symbol.m_base ? symbol.m_base
                                                      : symbol.m_label)
                        .x +
                    (symbol.m_script
                         ? ImGui::CalcTextSize(symbol.m_script).x * .65f
                         : 0);
                widest = std::max(widest, width);
            }
            const float cellWidth =
                std::max(touchHeight,
                         widest + ImGui::GetStyle().FramePadding.x * 2) +
                ImGui::GetStyle().ItemSpacing.x;
            const auto columns = static_cast<std::size_t>(std::max(
                1.f, std::floor(ImGui::GetContentRegionAvail().x / cellWidth)));
            for ( std::size_t i = 0; i < SYMBOLS.size(); ++i ) {
                if ( i % columns ) ImGui::SameLine();
                // 按钮先保存模板，输入回调再在原有选区插入。
                // 不能简单追加到末尾，复合公式需要在用户指定位置修改。
                // 模板字符串在按钮表内稳定拥有，队列只保存本次交互值。
                const bool clicked = mathSymbolButton(SYMBOLS[i], touchHeight);
                if ( clicked ) state.m_pendingInsertion = SYMBOLS[i].m_input;
                if ( !m_timingPresentation->symbolHelp(
                         SYMBOLS[i].m_input, SYMBOLS[i].m_help, clicked) &&
                     ImGui::IsItemHovered() ) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
                    ImGui::Text(TR("ui.timing.curve.symbol_expression").data(),
                                SYMBOLS[i].m_input);
                    ImGui::TextWrapped("%s", TR(SYMBOLS[i].m_help).data());
                    ImGui::TextUnformatted(
                        TR("ui.timing.curve.symbol_insert").data());
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }
            m_timingPresentation->finishSymbols();
            ImGui::TreePop();
        }
        // 点击按钮后重新聚焦表达式框，CallbackAlways 消费待插入模板。
        // 没有待插入内容时保持用户当前焦点，不妨碍手绘或参数输入。
        // 回调产生的文本修改也走同一编译入口，不单独执行模板。
        // 标签独占一行，输入框使用全宽时不能把右侧标签挤出窗口。
        ImGui::TextUnformatted(TR("ui.timing.curve.expression").data());
        const auto& style = ImGui::GetStyle();
        // 按输入框实际换行宽度估计高度，预留滚动条槽保持换行规则稳定。
        // 只改变显示布局，不能往数学源码插入换行或改变光标的字节偏移。
        // 表达式最多 2048 字节，换行度量有固定预算，不扫描谱面或完整时间线。
        // 窗口变窄后高度同步增加，未聚焦时也能看到拟合函数的完整后半部分。
        const float wrapWidth =
            std::max(1.f,
                     ImGui::GetContentRegionAvail().x -
                         2 * style.FramePadding.x - style.ScrollbarSize);
        const float inputHeight =
            std::max(ImGui::GetTextLineHeight() * 3,
                     ImGui::CalcTextSize(
                         state.m_expression.data(), nullptr, false, wrapWidth)
                         .y) +
            2 * style.FramePadding.y;
        if ( !state.m_pendingInsertion.empty() ) ImGui::SetKeyboardFocusHere();
        changed |= ImGui::InputTextMultiline(
            "##TimingFunctionExpression",
            state.m_expression.data(),
            state.m_expression.size(),
            ImVec2(-1, inputHeight),
            ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_WordWrap,
            mathInputCallback,
            &state);
        ImGui::TextWrapped(TR("ui.timing.curve.expression_help").data(),
                           curve.m_variable == TimingVariable::Beat
                               ? TR("ui.timing.curve.beats").data()
                               : TR("ui.timing.curve.seconds").data());
        // 相同表达式和相同时长沿用不可变缓存，预览不重复工作。
        // 改变时长仍需要重新证明全部定义域，即使文本没有变化。
        // 有效终点由 f(duration) 派生，不能独立修改成与函数矛盾的值。
        if ( changed || state.m_compiledDuration !=
                            timingInterpolationVariableDuration(curve) ) {
            // 编译只由真实编辑触发，失败保留文本和最后有效缓存。
            // 保存入口会检查错误说明，不允许旧函数冒充新的输入。
            state.m_compiledDuration =
                timingInterpolationVariableDuration(curve);
            setTimingInterpolationFunction(
                curve, state.m_expression.data(), edit.value, state.m_error);
        }
        if ( state.m_error.empty() && curve.m_function )
            renderTimingFormula(*curve.m_function);
        else
            ImGui::TextColored(
                ImVec4(1, .35f, .35f, 1), "%s", state.m_error.c_str());
    }
    // 手绘对所有预设开放，应用时再切换为自定义模式。
    // 折叠状态不销毁已经绘制的格点或候选。
    // 参考曲线只作视觉对照，不是拟合样本。
    if ( !ImGui::CollapsingHeader(TR("ui.timing.curve.sketch").data()) ) return;
    // 纵轴明确显示参数含义，不把鼠标像素直接当成 Timing 参数。
    // 轴值变化只改变视图；旧绘制点仍保存自己的实际参数值。
    m_timingPresentation->curveDouble(
        TR("ui.timing.curve.lower").data(), &state.m_minimum, "%.6g");
    m_timingPresentation->curveDouble(
        TR("ui.timing.curve.upper").data(), &state.m_maximum, "%.6g");
    // 除了上下界有限，还需检查差值有限；两个极端值之差可能溢出。
    // 无效纵轴立即结束面板绘图，禁止把 NaN 坐标提交渲染器。
    // 时间范围无效也不能从像素生成模型采样点。
    const bool validAxis = std::isfinite(state.m_minimum) &&
                           std::isfinite(state.m_maximum) &&
                           std::isfinite(state.m_maximum - state.m_minimum) &&
                           state.m_maximum > state.m_minimum;
    const bool validDuration =
        std::isfinite(timingInterpolationVariableDuration(curve)) &&
        timingInterpolationVariableDuration(curve) > 0 &&
        curve.m_duration >= .001;
    if ( !validAxis || !validDuration ) {
        ImGui::TextWrapped("%s",
                           TR("ui.timing.curve.invalid_drawing_axis").data());
        return;
    }
    // 区域宽度跟随窗口，绘制数据按归一横轴存储。
    // 窗口变宽或变窄不重采样已画曲线，也不会改变秒域含义。
    // 整个区域拥有共同剪裁框，纵轴外曲线不覆盖周围按钮。
    const ImVec2 size(
        std::max(180.f, ImGui::GetContentRegionAvail().x),
        m_timingPresentation->curveGraphHeight(
            200, Config::AppConfig::instance().getWindowContentScale()));
    // 图表的实际高度由平台策略提供，原桌面固定高度保持不变。
    // 子窗边界用于输入与裁剪隔离，不建立另一份拟合状态。
    // 父页滚动仍可从图外空白与普通组件起手。
    const bool surface =
        m_timingPresentation->beginCurveSurface("##TimingSketchSurface", size);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    auto*        draw   = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin,
                        ImVec2(origin.x + size.x, origin.y + size.y),
                        surface ? ImGui::GetColorU32(ImGuiCol_FrameBg)
                                : IM_COL32(18, 24, 32, 255),
                        surface ? ImGui::GetStyle().FrameRounding : 0.f);
    // 网格和参考线被裁剪到同一绘制区域，超出纵轴不污染周围控件。
    draw->PushClipRect(
        origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    for ( int i = 1; i < 5; ++i ) {
        const float ratio = i / 5.f;
        draw->AddLine(ImVec2(origin.x + size.x * ratio, origin.y),
                      ImVec2(origin.x + size.x * ratio, origin.y + size.y),
                      IM_COL32(60, 70, 80, 140));
        draw->AddLine(ImVec2(origin.x, origin.y + size.y * ratio),
                      ImVec2(origin.x + size.x, origin.y + size.y * ratio),
                      IM_COL32(60, 70, 80, 140));
    }
    if ( isValidTimingInterpolation(curve, edit.effect, edit.value) ) {
        // 暗色参考线只用于对照，未画过的格点不能参与拟合。
        // 显示分辨率不受手绘的 129 个格点限制，否则长段高频函数会混叠。
        // 拍域参考线按拍数取值，不能套用下方输出图的秒域均分。
        state.m_referencePreview.update(curve, edit.value, size.x, true);
        drawTimingInterpolationPreview(state.m_referencePreview,
                                       origin,
                                       size,
                                       state.m_minimum,
                                       state.m_maximum,
                                       IM_COL32(110, 145, 175, 100));
    }
    // InvisibleButton 是没有可见按钮外观的绘制命中区，不触发普通按钮样式。
    const bool touchSketch =
        m_timingPresentation->sketchInput(state, origin, size);
    if ( !touchSketch ) ImGui::InvisibleButton("函数自由绘制区域", size);
    // 仅左键绘制手势更新格点，鼠标离开后仍按有效范围夹取。
    // 每帧覆盖相邻旧点和新点之间的全部格点，不遗漏快速移动。
    // 鼠标方向倒转仍使用相同线性补齐规则。
    if ( !touchSketch && ImGui::IsItemActive() &&
         UI::Interaction::mouseDown(ImGuiMouseButton_Left) ) {
        const auto   mouse = ImGui::GetIO().MousePos;
        const double ratio = std::clamp(
            static_cast<double>((mouse.x - origin.x) / size.x), 0.0, 1.0);
        const auto   index = static_cast<int>(std::lround(ratio * 128));
        const double value = std::lerp(
            state.m_maximum,
            state.m_minimum,
            std::clamp(
                static_cast<double>((mouse.y - origin.y) / size.y), 0.0, 1.0));
        if ( state.m_previousIndex < 0 ) state.m_previousIndex = index;
        // 连续两帧之间的格点线性补齐，快速拖动和反向修改仍保持连续反馈。
        // 每次鼠标更新最多覆盖 129 点，没有时钟等待或跨线程命令。
        // 分母保留手势方向，反向移动的中间点仍落在两个端值之间。
        // 同一格点重复拖动直接采用新值，不能除以零。
        // 各格点的有效标记与参数一起更新，拟合只接收已绘制的段落。
        const auto low  = std::min(index, state.m_previousIndex),
                   high = std::max(index, state.m_previousIndex);
        for ( int i = low; i <= high; ++i ) {
            state.m_drawn[i] = true;
            state.m_values[i] =
                index == state.m_previousIndex
                    ? value
                    : std::lerp(state.m_previousValue,
                                value,
                                static_cast<double>(i - state.m_previousIndex) /
                                    (index - state.m_previousIndex));
        }
        state.m_previousIndex = index;
        state.m_previousValue = value;
        // 修改绘制点立即使拟合候选失效，不能应用旧图形的解。
        state.m_fit.reset();
        state.m_fitFunction.reset();
        state.m_fitError.clear();
    } else if ( !touchSketch )
        state.m_previousIndex = -1;
    // 只连接两个都有效的邻点，尚未覆盖区域必须保持可见空白。
    // 黄色线描述手绘意图，蓝色候选描述实际可保存函数。
    // 两条线使用相同投影和剪裁，误差对比不受坐标轴差异干扰。
    for ( std::size_t i = 1; i < state.m_values.size(); ++i )
        if ( state.m_drawn[i - 1] && state.m_drawn[i] )
            draw->AddLine(curvePoint(origin,
                                     size,
                                     (i - 1) / 128.0,
                                     state.m_values[i - 1],
                                     state.m_minimum,
                                     state.m_maximum),
                          curvePoint(origin,
                                     size,
                                     i / 128.0,
                                     state.m_values[i],
                                     state.m_minimum,
                                     state.m_maximum),
                          IM_COL32(255, 210, 90, 255),
                          2);
    if ( state.m_fitFunction ) {
        // 拟合结果用另一种颜色显示，用户比较误差后再决定是否保存段落。
        // 成功应用后就是同一函数，复用参考图的细节缓存，不重复稀疏求值。
        if ( curve.m_function == state.m_fitFunction )
            drawTimingInterpolationPreview(state.m_referencePreview,
                                           origin,
                                           size,
                                           state.m_minimum,
                                           state.m_maximum,
                                           IM_COL32(80, 205, 255, 255));
        else
            for ( int i = 1; i <= 128; ++i )
                draw->AddLine(
                    curvePoint(origin,
                               size,
                               (i - 1) / 128.0,
                               evaluateTimingFunction(
                                   *state.m_fitFunction,
                                   timingInterpolationVariableDuration(curve) *
                                       (i - 1) / 128),
                               state.m_minimum,
                               state.m_maximum),
                    curvePoint(origin,
                               size,
                               i / 128.0,
                               evaluateTimingFunction(
                                   *state.m_fitFunction,
                                   timingInterpolationVariableDuration(curve) *
                                       i / 128),
                               state.m_minimum,
                               state.m_maximum),
                    IM_COL32(80, 205, 255, 255),
                    2);
    }
    draw->PopClipRect();
    if ( surface ) ImGui::EndChild();
    if ( surface )
        ImGui::TextWrapped("%s", TR("ui.timing.curve.touch_sketch").data());
    ImGui::TextWrapped(TR("ui.timing.curve.drawing_axes").data(),
                       timingInterpolationVariableDuration(curve),
                       curve.m_variable == TimingVariable::Beat
                           ? TR("ui.timing.curve.beat_unit").data()
                           : TR("ui.timing.curve.second_unit").data(),
                       state.m_minimum,
                       state.m_maximum);
    ImGui::TextWrapped("%s", TR("ui.timing.curve.drawing_help").data());
    // 清空只影响手绘面板，现有函数和谱面内容保持不变。
    // 候选一并撤销，防止清空后仍应用旧手势的拟合结果。
    // 用户可重新完整绘制，再主动触发一次求解。
    const float touchHeight = m_timingPresentation->parameterButtonHeight(
        Config::AppConfig::instance().getWindowContentScale());
    const ImVec2 actionSize{ touchHeight > 0
                                 ? std::max(1.f,
                                            (ImGui::GetContentRegionAvail().x -
                                             ImGui::GetStyle().ItemSpacing.x) /
                                                2)
                                 : 0,
                             touchHeight };
    if ( UI::FeedbackButton(TR("ui.timing.curve.clear_drawing").data(),
                            actionSize) ) {
        state.m_drawn.fill(false);
        state.m_fit.reset();
        state.m_fitFunction.reset();
        state.m_fitError.clear();
    }
    ImGui::SameLine();
    // 每次求解先丢弃旧候选与错误，失败不会显示上次成功的解。
    // 输入固定 129 点，覆盖起终时刻且按时间递增。
    // 模型负责比较多种函数族并返回完整秒域表达式。
    if ( UI::FeedbackButton(TR("ui.timing.curve.fit").data(), actionSize) ) {
        state.m_fit.reset();
        state.m_fitFunction.reset();
        state.m_fitError.clear();
        if ( !std::all_of(state.m_drawn.begin(),
                          state.m_drawn.end(),
                          [](bool drawn) { return drawn; }) )
            state.m_fitError = TR("ui.timing.curve.incomplete").data();
        else {
            std::array<TimingCurvePoint, 129> points{};
            for ( std::size_t i = 0; i < points.size(); ++i )
                points[i] = { timingInterpolationVariableDuration(curve) * i /
                                  128,
                              state.m_values[i] };
            // 拟合结果经计算器再次编译，秩合法不代表完整域数学合法。
            // 模型返回 RMS 与最大误差，UI 不用相同拟合点冒充误差为零。
            // 合法拟合直接更新窗口副本，下方预览本帧即读取新函数。
            // 最终保存才发布逻辑命令，取消仍放弃全部中间修改。
            auto fit = fitTimingFunction(
                points, timingInterpolationVariableDuration(curve));
            if ( !fit )
                state.m_fitError = fit.error();
            else {
                auto compiled = compileTimingFunction(
                    fit->m_expression,
                    timingInterpolationVariableDuration(curve));
                if ( !compiled )
                    state.m_fitError = compiled.error();
                else {
                    state.m_fit         = std::move(*fit);
                    state.m_fitFunction = std::move(*compiled);
                    state.applyFit(curve, edit.effect, edit.value);
                }
            }
        }
    }
    // 时长修改后的旧拟合不能应用，必须针对新秒域重新求解。
    // 当前时长必须与候选一致，所有权存在不代表缓存仍适用。
    // 领域合法性取当前效果，不能沿用另一效果的 BPM 判断。
    // 最终保存仍由外层窗口检查范围碰撞和会话身份。
    const bool currentFit =
        state.m_fitFunction && timingFunctionDuration(*state.m_fitFunction) ==
                                   timingInterpolationVariableDuration(curve);
    if ( state.m_fit && currentFit ) {
        ImGui::Text(TR("ui.timing.curve.fit_errors").data(),
                    fitDisplayText(state.m_fit->m_family),
                    state.m_fit->m_rmsError,
                    state.m_fit->m_maxError);
        // 主公式和候选公式同时可见，必须用独立 ID 隔离滚动与子窗口。
        ImGui::PushID("拟合预览");
        renderTimingFormula(*state.m_fitFunction);
        ImGui::PopID();
        // 效果切换后重新采用当前领域限制，不沿用前一种效果的判断。
        const auto [minimum, maximum] =
            timingFunctionRange(*state.m_fitFunction);
        const bool invalidBpm = edit.effect == TimingEffect::BPM &&
                                (minimum < .1 || maximum > 10000);
        if ( invalidBpm )
            ImGui::TextColored(ImVec4(1, .35f, .35f, 1),
                               "%s",
                               TR("ui.timing.curve.bpm_fit_range").data());
        // 拟合只在按钮触发时应用，随后手动编辑公式不能被旧候选逐帧覆盖。
        // 仍保留显式恢复入口，便于比较预设或手改函数后重新采用该结果。
        if ( curve.m_curve == TimingCurve::Custom &&
             curve.m_function == state.m_fitFunction )
            ImGui::TextWrapped("%s", TR("ui.timing.curve.fit_applied").data());
        else {
            ImGui::BeginDisabled(!state.m_fitError.empty() || invalidBpm);
            if ( UI::FeedbackButton(TR("ui.timing.curve.use_fit").data()) )
                state.applyFit(curve, edit.effect, edit.value);
            ImGui::EndDisabled();
        }
    }
    if ( !state.m_fitError.empty() )
        ImGui::TextColored(
            ImVec4(1, .35f, .35f, 1), "%s", fitDisplayText(state.m_fitError));
}
}  // namespace MMM::Canvas
