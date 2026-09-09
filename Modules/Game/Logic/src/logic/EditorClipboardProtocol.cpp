#include "logic/EditorClipboardProtocol.h"
#include "mmm/SafeParse.h"
#include "mmm/note/Note.h"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace MMM::Logic::EditorClipboardProtocol
{
namespace
{
/// @brief 音符剪贴板条目的载荷类型码。
/// @note 头类别与正文 N 主行标签恰好相同，但分别由头解析器与载荷解析器处理。
constexpr std::string_view KIND_NOTES = "N";

/// @brief 混合谱面物件剪贴板条目的载荷类型码。
/// @note 该类别承载玩家音符和自动采样，不包含时间线效果条目。
constexpr std::string_view KIND_CHART_OBJECTS = "C";

/// @brief 时间线剪贴板条目的载荷类型码。
/// @note 使用单独的正文解析器，不能把时间点混入音符载荷后期待自动分类。
constexpr std::string_view KIND_TIMELINES = "T";

/// @brief 剪贴板载荷允许携带的单个协作逻辑标识最大字节数。
/// @note 此上限用于读取身份扩展，不是整份剪贴板大小或条目数量上限。
constexpr std::size_t MAX_COLLABORATION_ID_BYTES = 128U;
// 行标签与字段码属于文本协议，不使用 C++ 成员名或枚举整数作为线格式。
// 添加可选属性应使用独立扩展行；给既有固定字段行直接加列会被旧解析器拒绝。

/// @brief 追加一个制表符分隔符。
void appendSeparator(std::string& text)
{
    // 外层分隔与字段内容转义分工明确，不能在最终文本上统一替换制表符。
    text.push_back('\t');
}

/// @brief 追加一个换行符。
void appendLineBreak(std::string& text)
{
    // 输出统一使用 LF；跨平台读取兼容 CRLF，不随本机文本模式改变协议字节。
    text.push_back('\n');
}

/// @brief 将一个十六进制半字节转换为可打印字符。
/// @param value 已限制在 0～15 的半字节值。
/// @return 大写十六进制字符，供百分号转义使用。
char hexDigit(unsigned int value)
{
    // 上游已经通过移位或掩码取得半字节，这里不再次修正越界输入。
    return static_cast<char>(value < 10U ? ('0' + value) : ('A' + value - 10U));
}

/// @brief 将一个可打印十六进制字符转换为半字节。
/// @param ch 百分号后的一位编码字符。
/// @return 数值；非十六进制字符返回空值。
std::optional<unsigned int> hexValue(char ch)
{
    // 仅识别 ASCII 数字，不受当前语言环境的字符分类规则影响。
    if ( ch >= '0' && ch <= '9' ) {
        return static_cast<unsigned int>(ch - '0');
    }
    if ( ch >= 'A' && ch <= 'F' ) {
        return static_cast<unsigned int>(ch - 'A' + 10);
    }
    if ( ch >= 'a' && ch <= 'f' ) {
        // 接受外部文本的小写转义，序列化仍统一输出大写形式。
        return static_cast<unsigned int>(ch - 'a' + 10);
    }
    return std::nullopt;
}

/// @brief 使用百分号转义追加可安全包含制表符和换行的元数据字符串。
/// @param text 追加写入的协议文本。
/// @param value 原始字节串，不要求转换 Unicode 编码。
/// @note 不是 URL 编码：空格、加号和非 ASCII 字节均原样输出。
/// @pre value 与 text 不共享可能被追加操作重分配的内部存储。
void appendEscapedField(std::string& text, std::string_view value)
{
    // value 不应借用 text 的内部存储，追加扩容可能使正在遍历的视图失效。
    // 调用点传入组件字段，保持输入对象与输出缓冲区的生命周期独立。
    for ( unsigned char ch : value ) {
        // 按无符号字节拆半字节，避免非 ASCII 字符经有符号 char 右移改变结果。
        // 协议分隔字符与转义引导符必须编码，其他字节保持原样。
        if ( ch == '%' || ch == '\t' || ch == '\n' || ch == '\r' ) {
            text.push_back('%');
            text.push_back(hexDigit(ch >> 4U));
            text.push_back(hexDigit(ch & 0x0FU));
        } else {
            text.push_back(static_cast<char>(ch));
        }
    }
}

/// @brief 解码一个百分号转义的元数据字段。
/// @param value 已从行中拆出的单个字段，不包含字段分隔符。
/// @return 解码后的字节串；残缺或非法转义使整个字段失败。
/// @note 不校验 UTF-8，也不拒绝编码得到的零字节；字段用途的约束由上层检查。
/// @note 接受任意合法的两位十六进制转义，不限于输出端主动转义的四种字符。
std::optional<std::string> decodeEscapedField(std::string_view value)
{
    std::string result;
    // 先在局部构造完整字段，失败直接丢弃，调用方不会收到半解码字符串。
    result.reserve(value.size());
    // 转义只会缩短长度，以输入长度预留即可容纳完整解码结果。
    for ( std::size_t index = 0; index < value.size(); ++index ) {
        const char ch = value[index];
        if ( ch != '%' ) {
            result.push_back(ch);
            continue;
        }
        if ( index + 2 >= value.size() ) {
            // 百分号必须带两位十六进制数，不把残缺编码当作普通文字保留。
            return std::nullopt;
        }
        const auto high = hexValue(value[index + 1]);
        const auto low  = hexValue(value[index + 2]);
        if ( !high || !low ) {
            return std::nullopt;
        }
        result.push_back(static_cast<char>(((*high) << 4U) | (*low)));
        // 此处只还原字节，不把还原出的换行重新送回行拆分器。
        index += 2;
        // 跳过已经消费的两位编码，解码出的百分号不会再次递归展开。
    }
    return result;
}

/// @brief 追加一个整数字段。
/// @note 不附带字段分隔符，调用方负责固定列数与字段顺序。
void appendIntField(std::string& text, int value)
{
    text += fmt::format("{}", value);
}

/// @brief 追加一个有符号 64 位整数字段。
/// @note 保持整数表示，不能经 double 格式化而损失大偏移的低位精度。
void appendInt64Field(std::string& text, std::int64_t value)
{
    text += fmt::format("{}", value);
}

/// @brief 追加一个无符号 32 位整数字段。
/// @note 使用十进制文本，不输出依赖平台字节序的二进制轨号。
void appendUint32Field(std::string& text, std::uint32_t value)
{
    text += fmt::format("{}", value);
}

/// @brief 以 0 或 1 追加一个布尔字段。
/// @note 固定协议拼写，不受界面语言或布尔文本格式偏好影响。
void appendBoolField(std::string& text, bool value)
{
    text.push_back(value ? '1' : '0');
}

/// @brief 追加一个紧凑浮点数字段。
/// @param text 协议输出缓冲区。
/// @param value 待输出数值；非有限值规范为零。
/// @note 不改变有限负值或数值单位，业务范围限制不属于通用字段输出职责。
void appendDoubleField(std::string& text, double value)
{
    if ( !std::isfinite(value) ) {
        // 写端使用可解析的后备值，读端则拒绝非法浮点文本；两端不是对称修复器。
        // 输出协议不携带 NaN 或无穷，避免接收端得到不可用于坐标运算的值。
        text.push_back('0');
        return;
    }
    // 17 位有效数字满足 double 往返精度，同时避免固定小数位产生长尾零。
    text += fmt::format("{:.17g}", value);
}

/// @brief 解析一个整数字段。
/// @return 完整字段对应的 int；溢出、无数字或尾随内容均返回空值。
/// @note 不裁剪空白；字段前后的空格属于输入内容而非可忽略的排版。
std::optional<int> parseIntField(std::string_view field)
{
    int value = 0;
    auto [ptr, ec] =
        std::from_chars(field.data(), field.data() + field.size(), value);
    if ( ec != std::errc{} || ptr != field.data() + field.size() ) {
        // from_chars 可只解析前缀，协议要求整个字段耗尽才接受结果。
        return std::nullopt;
    }
    return value;
}

/// @brief 解析一个有符号 64 位整数字段。
/// @note 保留有符号范围，用于可能为负的毫秒偏移，不经浮点中转。
/// @return 完整解析的值，或表示格式/范围错误的空值。
std::optional<std::int64_t> parseInt64Field(std::string_view field)
{
    // 偏移量可在时间锚点之前，不采用无符号解析再做强制转换。
    std::int64_t value = 0;
    auto [ptr, ec] =
        std::from_chars(field.data(), field.data() + field.size(), value);
    if ( ec != std::errc{} || ptr != field.data() + field.size() ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 解析一个无符号 32 位整数字段。
/// @note 直接解析目标类型，由 from_chars 拒绝负数与超范围轨道编号。
/// @return 完整解析的无符号值；不接受合法数字后附带的文本。
std::optional<std::uint32_t> parseUint32Field(std::string_view field)
{
    // 协议容量校验与目标谱面轨数校验分开，成功不表示该轨已在目标中创建。
    std::uint32_t value = 0;
    auto [ptr, ec] =
        std::from_chars(field.data(), field.data() + field.size(), value);
    if ( ec != std::errc{} || ptr != field.data() + field.size() ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 解析一个布尔字段。
/// @return 仅接受协议规定的 0、1，其他拼写均视为非法字段。
/// @note optional 的 false 值是成功结果，与空 optional 必须区分。
std::optional<bool> parseBoolField(std::string_view field)
{
    if ( field == "0" ) return false;
    if ( field == "1" ) return true;
    return std::nullopt;
}

/// @brief 解析一个有限浮点数字段。
/// @return 完整且有限的数值，失败不以零值掩盖格式问题。
/// @note 不按用途钳制颜色、音量或时间，具体行解析器在此基础上施加范围约束。
std::optional<double> parseDoubleField(std::string_view field)
{
    const auto result = Internal::parseFloatingPrefix(field);
    // 保留科学计数法等底层支持的表示，但不能因前缀有效就吞掉末尾损坏内容。
    // 底层工具允许前缀解析，协议边界追加全长和有限性约束。
    if ( result.error != std::errc{} || result.parsedLength != field.size() ||
         !std::isfinite(result.value) ) {
        return std::nullopt;
    }
    return result.value;
}

/// @brief 拆分一行以制表符分隔的协议文本。
/// @param line 已移除行尾的输入视图。
/// @return 借用输入存储的字段视图，调用方不得让其超出原文本生命周期。
/// @note 空行得到一个空字段；是否忽略空行由载荷解析入口决定。
std::vector<std::string_view> splitFields(std::string_view line)
{
    std::vector<std::string_view> fields;
    // 这里只分配视图数组，字段字节仍由整份输入拥有；需要保存时再显式解码复制。
    std::size_t start = 0;
    while ( start <= line.size() ) {
        // 使用 <= 保留末尾空字段，字段数量校验依赖它而不是忽略尾分隔符。
        const std::size_t end = line.find('\t', start);
        if ( end == std::string_view::npos ) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, end - start));
        // 先按原始分隔符拆分，百分号编码的制表符应在字段解码时才还原。
        start = end + 1;
    }
    return fields;
}

/// @brief 从字符串视图读取并移除一行。
/// @param text 未消费文本；返回后前缀推进到下一行。
/// @return 借用原始存储的行视图；输入已空时返回空值。
/// @note 即使后续字段校验失败也已消费该行，解析器不会自动回退。
std::optional<std::string_view> popLine(std::string_view& text)
{
    if ( text.empty() ) {
        return std::nullopt;
    }

    const std::size_t end = text.find('\n');
    // 单独的 CR 不是行分隔符，只有行尾兼容处理才会移除它。
    std::string_view line;
    if ( end == std::string_view::npos ) {
        // 接受末行没有换行符的外部剪贴板文本，并将剩余输入一次消费完。
        line = text;
        text = {};
    } else {
        line = text.substr(0, end);
        text.remove_prefix(end + 1);
    }
    if ( line.ends_with('\r') ) {
        // 只剥离末尾一个回车，不修剪字段两侧空格或中间的其他控制字节。
        // 同时支持 LF 和 CRLF，字段内容中的回车应由百分号转义表示。
        line.remove_suffix(1);
    }
    return line;
}

/// @brief 将音符类型转换为单字符协议码。
/// @note 协议码独立于枚举数值，调整内部枚举次序不会改变文本格式。
/// @pre 调用方应提供支持的类型；当前未知枚举的输出后备为普通音符码。
std::string_view noteTypeCode(::MMM::NoteType type)
{
    switch ( type ) {
    case ::MMM::NoteType::NOTE: return "n";
    case ::MMM::NoteType::HOLD: return "h";
    case ::MMM::NoteType::FLICK: return "f";
    case ::MMM::NoteType::POLYLINE: return "p";
    }
    return "n";
}

/// @brief 将单字符协议码转换为音符类型。
/// @return 已知类型；未知码不按普通音符静默接受。
std::optional<::MMM::NoteType> noteTypeFromCode(std::string_view code)
{
    // 必须匹配完整字段；例如带额外后缀的 n 不是普通音符的兼容别名。
    if ( code == "n" ) return ::MMM::NoteType::NOTE;
    if ( code == "h" ) return ::MMM::NoteType::HOLD;
    if ( code == "f" ) return ::MMM::NoteType::FLICK;
    if ( code == "p" ) return ::MMM::NoteType::POLYLINE;
    return std::nullopt;
}

/// @brief 将时间线效果转换为单字符协议码。
/// @note 使用独立的效果码表，字符 h 在本领域表示 HS 而非长条。
/// @pre 调用方应提供支持的效果；未知枚举的输出后备为 SCROLL 码。
std::string_view timingEffectCode(::MMM::TimingEffect effect)
{
    switch ( effect ) {
    case ::MMM::TimingEffect::BPM: return "b";
    case ::MMM::TimingEffect::SCROLL: return "s";
    case ::MMM::TimingEffect::JUMP: return "j";
    case ::MMM::TimingEffect::HS: return "h";
    }
    return "s";
}

/// @brief 将单字符协议码转换为时间线效果。
/// @return 已知效果；未知码使所属时间点字段校验失败。
std::optional<::MMM::TimingEffect> timingEffectFromCode(std::string_view code)
{
    // 同一字符在不同字段可有不同含义，只能在时间线效果列调用此码表。
    if ( code == "b" ) return ::MMM::TimingEffect::BPM;
    if ( code == "s" ) return ::MMM::TimingEffect::SCROLL;
    if ( code == "j" ) return ::MMM::TimingEffect::JUMP;
    if ( code == "h" ) return ::MMM::TimingEffect::HS;
    return std::nullopt;
}

/// @brief 将音符元数据来源转换为紧凑协议码。
/// @param type 原属性所属谱面格式，不是当前粘贴目标格式。
/// @return 静态字符串视图；未支持的枚举值返回空视图。
std::string_view noteMetadataSourceCode(::MMM::NoteMetadataType type)
{
    // 返回静态字面量，无需为每条元数据分配来源名，也不借用被遍历的属性容器。
    switch ( type ) {
    case ::MMM::NoteMetadataType::OSU: return "o";
    case ::MMM::NoteMetadataType::MALODY: return "ma";
    case ::MMM::NoteMetadataType::RM: return "r";
    case ::MMM::NoteMetadataType::MMM: return "m";
    }
    return {};
}

/// @brief 将紧凑协议码转换为音符元数据来源。
/// @param code 区分大小写的来源字段。
/// @return 已支持的来源枚举，未知来源不归入默认格式。
std::optional<::MMM::NoteMetadataType> noteMetadataSourceFromCode(
    std::string_view code)
{
    // MALODY 与 MMM 分别使用 ma、m，不能只看首字符就归并来源。
    if ( code == "o" ) return ::MMM::NoteMetadataType::OSU;
    if ( code == "ma" ) return ::MMM::NoteMetadataType::MALODY;
    if ( code == "r" ) return ::MMM::NoteMetadataType::RM;
    if ( code == "m" ) return ::MMM::NoteMetadataType::MMM;
    return std::nullopt;
}

/// @brief 将时间线元数据来源转换为紧凑协议码。
/// @return 时间线来源码；无映射时由属性输出流程跳过该来源。
std::string_view timingMetadataSourceCode(::MMM::TimingMetadataType type)
{
    // 时间线没有复用音符的 MMM 来源；扩展新来源时须成对更新读写码表。
    switch ( type ) {
    case ::MMM::TimingMetadataType::OSU: return "o";
    case ::MMM::TimingMetadataType::RM: return "r";
    case ::MMM::TimingMetadataType::MALODY: return "ma";
    }
    return {};
}

/// @brief 将紧凑协议码转换为时间线元数据来源。
/// @note 只接受时间线领域已支持的来源，不复用音符来源的全部取值。
std::optional<::MMM::TimingMetadataType> timingMetadataSourceFromCode(
    std::string_view code)
{
    if ( code == "o" ) return ::MMM::TimingMetadataType::OSU;
    if ( code == "r" ) return ::MMM::TimingMetadataType::RM;
    if ( code == "ma" ) return ::MMM::TimingMetadataType::MALODY;
    return std::nullopt;
}

/// @brief 将自动采样元数据来源转换为紧凑协议码。
/// @return MALODY 或 MMM 来源码；其他枚举值无输出映射。
std::string_view sampleMetadataSourceCode(::MMM::SampleMetadataType type)
{
    // 来源表示属性如何解释，不决定采样音频资源应从哪个目录加载。
    switch ( type ) {
    case ::MMM::SampleMetadataType::MALODY: return "ma";
    case ::MMM::SampleMetadataType::MMM: return "m";
    }
    return {};
}

/// @brief 将紧凑协议码转换为自动采样元数据来源。
/// @return 支持的样本来源；无映射时使该扩展属性行失效。
std::optional<::MMM::SampleMetadataType> sampleMetadataSourceFromCode(
    std::string_view code)
{
    if ( code == "ma" ) return ::MMM::SampleMetadataType::MALODY;
    if ( code == "m" ) return ::MMM::SampleMetadataType::MMM;
    return std::nullopt;
}

/// @brief 颜色存在时追加一行颜色覆盖。
/// @param text 输出协议文本。
/// @param prefix 主音符或子节点的颜色行类型。
/// @param code 颜色作用部位的协议码。
/// @param color 可选覆盖值，缺失表示继续使用默认颜色。
/// @note 四通道按独立浮点输出，不乘 alpha，也不做色彩空间变换。
void appendColorLine(std::string& text, std::string_view prefix,
                     std::string_view                code,
                     const std::optional<glm::vec4>& color)
{
    if ( !color ) {
        // 缺失和全透明不同：缺失省略整行，全透明仍需输出 alpha 为零的覆盖。
        return;
    }

    text.append(prefix);
    appendSeparator(text);
    text.append(code);
    // 部位码保持原样，外层选择 NC 或 SC 才决定颜色属于父音符还是子节点。
    appendSeparator(text);
    appendDoubleField(text, color->r);
    appendSeparator(text);
    appendDoubleField(text, color->g);
    appendSeparator(text);
    appendDoubleField(text, color->b);
    appendSeparator(text);
    appendDoubleField(text, color->a);
    // Alpha 与 RGB 一起持久化，不能因当前皮肤不使用透明度而省略该字段。
    appendLineBreak(text);
}

/// @brief 追加音符颜色覆盖行。
/// @param text 输出文本。
/// @param prefix 决定颜色属于主音符还是当前子节点。
/// @param colors 各物件部位独立的可选覆盖。
/// @note 固定输出部位顺序，不根据音符类型隐藏暂时未使用的颜色覆盖。
void appendNoteColorLines(std::string& text, std::string_view prefix,
                          const NoteColorOverrides& colors)
{
    appendColorLine(text, prefix, "t", colors.tap);
    // 部位码与 assignColor 对应，长条身体和头尾分开保留，不能合并成单色。
    appendColorLine(text, prefix, "h", colors.head);
    appendColorLine(text, prefix, "b", colors.hold);
    appendColorLine(text, prefix, "e", colors.end);
    appendColorLine(text, prefix, "f", colors.flickArrow);
    appendColorLine(text, prefix, "n", colors.node);
}

/// @brief 解析一行颜色覆盖。
/// @param fields 行类型、部位码和四个颜色分量，共六列。
/// @return 规范到单位区间的 RGBA 值；缺列或无效数值返回空值。
/// @note 此函数只解析颜色值，部位码由 assignColor 解释。
/// @note 零 alpha 是有效结果，调用方不能以透明度判断是否存在覆盖。
std::optional<glm::vec4> parseColorLine(
    const std::vector<std::string_view>& fields)
{
    if ( fields.size() != 6 ) {
        return std::nullopt;
    }

    const auto r = parseDoubleField(fields[2]);
    // 行类型和部位码由调用方消费，这里不能把第二列误当成红通道。
    const auto g = parseDoubleField(fields[3]);
    const auto b = parseDoubleField(fields[4]);
    const auto a = parseDoubleField(fields[5]);
    // 四通道均有效才构造颜色，不能用默认通道掩盖损坏的覆盖数据。
    if ( !r || !g || !b || !a ) {
        // 不部分更新目标颜色，全部通道通过后调用方才会执行 assignColor。
        return std::nullopt;
    }

    return glm::vec4{
        // 对超出显示范围的分量逐通道限制，保留其余有效分量而非丢弃整行。
        std::clamp(static_cast<float>(*r), 0.0F, 1.0F),
        std::clamp(static_cast<float>(*g), 0.0F, 1.0F),
        std::clamp(static_cast<float>(*b), 0.0F, 1.0F),
        std::clamp(static_cast<float>(*a), 0.0F, 1.0F),
    };
}

/// @brief 将解析出的颜色覆盖写入目标颜色集合。
/// @param colors 当前物件的可选颜色集合。
/// @param code 颜色部位的协议码，而非音符类型码。
/// @param color 已由颜色行解析器校验的 RGBA 值。
/// @note 未知部位码不更新任何覆盖；同一已知部位重复出现时最后有效值生效。
void assignColor(NoteColorOverrides& colors, std::string_view code,
                 const glm::vec4& color)
{
    if ( code == "t" ) {
        // 各部位独立覆盖，缺失的颜色仍保留皮肤默认行为。
        colors.tap = color;
    } else if ( code == "h" ) {
        colors.head = color;
    } else if ( code == "b" ) {
        colors.hold = color;
    } else if ( code == "e" ) {
        colors.end = color;
    } else if ( code == "f" ) {
        colors.flickArrow = color;
    } else if ( code == "n" ) {
        colors.node = color;
    }
}

/// @brief 追加音符元数据属性行。
/// @param text 协议输出文本。
/// @param prefix 指明主音符或子节点归属的行类型。
/// @param metadata 按来源格式分组的原始扩展属性。
/// @note 只传输属性字典；不在此解释、迁移或归一化各格式专有值。
void appendNoteMetadataLines(std::string& text, std::string_view prefix,
                             const ::MMM::NoteMetadata& metadata)
{
    for ( const auto& [source, properties] : metadata.note_properties ) {
        // 元数据按既有容器迭代顺序输出，不额外排序或改变源属性集合。
        const auto sourceCode = noteMetadataSourceCode(source);
        // 无已知协议码的来源不输出，不能用空码冒充另一种格式。
        if ( sourceCode.empty() ) {
            continue;
        }

        for ( const auto& [key, value] : properties ) {
            // 一条属性独占一行，键和值分别转义，保留其中的制表符和换行。
            text.append(prefix);
            appendSeparator(text);
            text.append(sourceCode);
            appendSeparator(text);
            appendEscapedField(text, key);
            appendSeparator(text);
            appendEscapedField(text, value);
            appendLineBreak(text);
        }
    }
}

/// @brief 追加时间线元数据属性行。
/// @param text 协议输出文本。
/// @param prefix 时间线属性的行类型标识。
/// @param metadata 各来源格式的时间线扩展属性。
/// @note 元数据来源可与目标导出格式不同，不能只保留当前目标格式的一组属性。
void appendTimingMetadataLines(std::string& text, std::string_view prefix,
                               const ::MMM::TimingMetadata& metadata)
{
    for ( const auto& [source, properties] : metadata.timing_properties ) {
        // 空属性组没有独立行，往返文本只保留实际键值，不保留空组的存在性。
        // 来源随每条属性输出，接收端无需从先前属性行继承隐式格式状态。
        const auto sourceCode = timingMetadataSourceCode(source);
        if ( sourceCode.empty() ) {
            continue;
        }

        for ( const auto& [key, value] : properties ) {
            text.append(prefix);
            appendSeparator(text);
            text.append(sourceCode);
            appendSeparator(text);
            appendEscapedField(text, key);
            appendSeparator(text);
            appendEscapedField(text, value);
            appendLineBreak(text);
        }
    }
}

/// @brief 追加自动采样元数据属性行。
/// @param text 协议输出文本。
/// @param prefix 自动采样属性的行类型标识。
/// @param metadata 自动采样原始扩展属性，不包含资源文件本身。
/// @note 不把资源 ID 补入属性字典，资源引用已经由采样主行单独编码。
void appendSampleMetadataLines(std::string& text, std::string_view prefix,
                               const ::MMM::SampleMetadata& metadata)
{
    for ( const auto& [source, properties] : metadata.sample_properties ) {
        // 属性值是协议文本而不是音频二进制，不在这一步打开或探测资源文件。
        // 使用采样专用来源码表，不假定其可用来源与音符或时间线相同。
        const auto sourceCode = sampleMetadataSourceCode(source);
        if ( sourceCode.empty() ) {
            continue;
        }

        for ( const auto& [key, value] : properties ) {
            text.append(prefix);
            appendSeparator(text);
            text.append(sourceCode);
            appendSeparator(text);
            appendEscapedField(text, key);
            appendSeparator(text);
            appendEscapedField(text, value);
            appendLineBreak(text);
        }
    }
}

/// @brief 将一行音符元数据属性解析到元数据容器中。
/// @param fields 行类型、来源、键、值四个原始字段。
/// @param metadata 当前音符的目标元数据集合。
/// @note 非法扩展行被忽略，不删除此前已解析的物件或属性。
/// @note 解析器只追加或覆盖键值，没有用缺失字段删除已有属性的操作语义。
void parseNoteMetadataLine(const std::vector<std::string_view>& fields,
                           ::MMM::NoteMetadata&                 metadata)
{
    if ( fields.size() != 4 ) {
        return;
    }

    const auto source = noteMetadataSourceFromCode(fields[1]);
    // 键和值都允许为空；optional 的存在性判断不等价于字符串非空判断。
    const auto key   = decodeEscapedField(fields[2]);
    const auto value = decodeEscapedField(fields[3]);
    if ( !source || !key || !value ) {
        return;
    }
    metadata.note_properties[*source][*key] = *value;
    // 同一来源和键重复出现时以后值覆盖前值，不叠加为多值属性。
}

/// @brief 将一行时间线元数据属性解析到元数据容器中。
/// @param fields 已按制表符拆出的四个字段。
/// @param metadata 当前时间点的目标属性集合。
/// @note 未知来源或坏转义只影响本行，不使已经接受的时间点失效。
void parseTimingMetadataLine(const std::vector<std::string_view>& fields,
                             ::MMM::TimingMetadata&               metadata)
{
    if ( fields.size() != 4 ) {
        return;
    }

    const auto source = timingMetadataSourceFromCode(fields[1]);
    // 来源码不做百分号解码，只有承载任意文本的键值列采用转义规则。
    const auto key   = decodeEscapedField(fields[2]);
    const auto value = decodeEscapedField(fields[3]);
    if ( !source || !key || !value ) {
        return;
    }
    metadata.timing_properties[*source][*key] = *value;
    // 按来源隔离同名键，避免不同谱面格式的扩展属性相互覆盖。
}

/// @brief 将一行自动采样元数据属性解析到元数据容器中。
/// @param fields 包含来源码与转义键值的完整属性行。
/// @param metadata 当前样本的目标属性集合。
/// @note 写入前完成三项解码校验，避免坏字段意外创建空来源组。
void parseSampleMetadataLine(const std::vector<std::string_view>& fields,
                             ::MMM::SampleMetadata&               metadata)
{
    if ( fields.size() != 4 ) {
        return;
    }

    const auto source = sampleMetadataSourceFromCode(fields[1]);
    // 属性名不按业务白名单筛选，已知来源下的未知键仍保留以支持格式往返。
    const auto key   = decodeEscapedField(fields[2]);
    const auto value = decodeEscapedField(fields[3]);
    if ( !source || !key || !value ) {
        return;
    }
    metadata.sample_properties[*source][*key] = *value;
    // 重复来源与键更新为最后一个有效值；后续坏行不会撤销此前接受的值。
    // 字段全部有效后才写入，失败行不会产生半初始化的属性条目。
}

/// @brief 追加一个数字列表字段。
/// @param text 输出缓冲区，本函数不追加外层制表符或换行。
/// @param values 保持原顺序的数值列表。
/// @note 重复数值照常输出，列表不是集合，元素次序用于关联折线节点。
void appendNumberListField(std::string& text, const std::vector<double>& values)
{
    // 逗号只用在数值列表内部，外层仍保持单列；不能复用制表符分隔列表元素。
    for ( std::size_t index = 0; index < values.size(); ++index ) {
        if ( index > 0 ) {
            // 分隔符只放在元素之间，空列表编码为空字段，不额外输出占位数值。
            text.push_back(',');
        }
        appendDoubleField(text, values[index]);
    }
}

/// @brief 解析一个逗号分隔的数字列表字段。
/// @param field 单个列表字段，不含外层制表符。
/// @return 保留有效数值的顺序；空项和无效项被跳过，不插入零占位。
/// @note 返回列表可能短于输入项数，使用者不能假定原始元素位置全部保留。
/// @note 没有区间展开语法，单个 token 必须是一个完整有限浮点数。
std::vector<double> parseNumberListField(std::string_view field)
{
    // 空列表与仅含无效项的列表都会得到空结果，没有另设“列表损坏”状态。
    std::vector<double> values;
    std::size_t         start = 0;
    while ( start <= field.size() ) {
        const std::size_t end   = field.find(',', start);
        const auto        token = field.substr(
            start,
            end == std::string_view::npos ? field.size() - start : end - start);
        if ( !token.empty() ) {
            // 列表采用逐项容错，而不是一个错误就丢弃全部已解析数值。
            if ( auto value = parseDoubleField(token) ) {
                values.push_back(*value);
            }
        }
        if ( end == std::string_view::npos ) {
            break;
        }
        start = end + 1;
    }
    return values;
}

/// @brief 追加一行主音符数据。
/// @param text 输出协议文本。
/// @param note 主记录的组件快照；子节点及可选属性由后续行编码。
/// @note 字段顺序属于协议契约，不能随内部成员布局调整。
/// @note 这里只编码基础行，不保存源 Registry 的父实体句柄或选择状态。
void appendMainNoteLine(std::string& text, const NoteComponent& note)
{
    // 主音符时间和时长来自 ECS，单位为秒，不使用谱面文件模型的毫秒表示。
    text.append("N");
    appendSeparator(text);
    text.append(noteTypeCode(note.m_type));
    appendSeparator(text);
    appendDoubleField(text, note.m_timestamp);
    appendSeparator(text);
    appendDoubleField(text, note.m_duration);
    // 时间与轨道保留组件中的逻辑数值，粘贴定位变换不在序列化阶段执行。
    appendSeparator(text);
    appendIntField(text, note.m_trackIndex);
    // 有符号轨号保留草稿轨编码，写端不能先转为无符号数丢失负轨含义。
    appendSeparator(text);
    appendIntField(text, note.m_dtrack);
    appendSeparator(text);
    appendBoolField(text, note.m_isSubNote);
    appendSeparator(text);
    appendIntField(text, note.m_subIndex);
    appendLineBreak(text);
}

/// @brief 追加一行子音符数据。
/// @param text 输出协议文本。
/// @param subNote 归属于最近一个主音符的子节点快照。
/// @note 子行不独立携带父 ID，其归属由载荷中的行顺序确定。
/// @note 内嵌节点是值快照，不递归序列化另一层子节点树。
void appendSubNoteLine(std::string& text, const NoteComponent::SubNote& subNote)
{
    // 子节点同样保存绝对会话时间，不减主音符时间构造另一套局部坐标。
    text.append("S");
    appendSeparator(text);
    text.append(noteTypeCode(subNote.type));
    appendSeparator(text);
    appendDoubleField(text, subNote.timestamp);
    appendSeparator(text);
    appendDoubleField(text, subNote.duration);
    appendSeparator(text);
    appendIntField(text, subNote.trackIndex);
    appendSeparator(text);
    appendIntField(text, subNote.dtrack);
    // 本行只保存基础几何属性，颜色、采样绑定和元数据使用独立扩展行。
    appendLineBreak(text);
}

/// @brief 为一个复制音符追加可选拍位数据。
/// @param text 输出协议文本。
/// @param item 含复制时拍位缓存的条目。
/// @note 不现场查询 BPM；拍位与时间的对应关系须由复制流程提前计算。
/// @note 起止拍位可相同，零长度物件的节奏定位不应被当作缺失数据。
void appendBeatLine(std::string& text, const ClipboardItem& item)
{
    if ( !item.hasBeatPositions ) {
        // 缺少拍位缓存时省略整行，不能把缺省零值误报为有效节奏定位。
        return;
    }

    text.append("NB");
    appendSeparator(text);
    appendDoubleField(text, item.startBeat);
    appendSeparator(text);
    appendDoubleField(text, item.endBeat);
    appendSeparator(text);
    appendNumberListField(text, item.subStartBeats);
    // 节点拍位列表沿用复制快照顺序，不根据当前节点时间重新计算或排序。
    appendSeparator(text);
    appendNumberListField(text, item.subEndBeats);
    appendLineBreak(text);
}

/// @brief 追加一个复制音符及其辅助数据行。
/// @param text 目标文本。
/// @param item 待序列化条目。
/// @param preserveCollaborationIdentity 是否写出协作稳定身份。
/// @note 输出身份和批注不做长度截断；接收端按各自字段上限决定是否接受。
/// @pre 快照字段已由复制流程准备，输出期间不得并发修改主物件及其节点数组。
void appendClipboardItem(std::string& text, const ClipboardItem& item,
                         bool preserveCollaborationIdentity)
{
    // 先建立主行上下文，紧随其后的扩展行才有明确的归属对象。
    appendMainNoteLine(text, item.note);
    if ( preserveCollaborationIdentity &&
         !item.note.m_collaborationId.empty() ) {
        // 身份保留由调用方显式选择，普通复制不应让新物件冒用来源身份。
        text.append("NI");
        appendSeparator(text);
        appendEscapedField(text, item.note.m_collaborationId);
        appendLineBreak(text);
    }
    if ( !item.note.m_annotation.empty() ) {
        // 这里传输物件的兼容文本字段，不是独立批注表记录及其作者、记录身份。
        text.append("NA");
        appendSeparator(text);
        appendEscapedField(text, item.note.m_annotation);
        appendLineBreak(text);
    }
    if ( item.note.m_sampleBinding &&
         !item.note.m_sampleBinding->m_audioResourceId.empty() ) {
        // 绑定记录包含资源 ID 和独立音量，不将资源内容内嵌到剪贴板。
        text.append("NS");
        appendSeparator(text);
        appendEscapedField(text, item.note.m_sampleBinding->m_audioResourceId);
        appendSeparator(text);
        appendDoubleField(text, item.note.m_sampleBinding->m_volume);
        appendLineBreak(text);
    }
    appendBeatLine(text, item);
    // 拍位属于整个主条目，先于子行输出，不以某个子节点的出现切换归属。
    appendNoteColorLines(text, "NC", item.note.m_customColors);
    appendNoteMetadataLines(text, "NM", item.note.m_metadata);

    for ( const auto& subNote : item.note.m_subNotes ) {
        // 保持连接顺序，不按时间排序；折线首尾角色依赖节点数组顺序。
        // 每个子节点及其扩展必须连续输出，解析器把扩展附到最近的子节点。
        appendSubNoteLine(text, subNote);
        if ( preserveCollaborationIdentity &&
             !subNote.collaborationId.empty() ) {
            text.append("SI");
            appendSeparator(text);
            appendEscapedField(text, subNote.collaborationId);
            appendLineBreak(text);
        }
        if ( !subNote.annotation.empty() ) {
            // 子批注不提升为父批注，允许同一折线各节点持有不同文本。
            text.append("SA");
            appendSeparator(text);
            appendEscapedField(text, subNote.annotation);
            appendLineBreak(text);
        }
        if ( subNote.sampleBinding &&
             !subNote.sampleBinding->m_audioResourceId.empty() ) {
            // 只写显式节点绑定，不把播放阶段可能采用的父绑定后备物化到文本中。
            text.append("SS");
            appendSeparator(text);
            appendEscapedField(text, subNote.sampleBinding->m_audioResourceId);
            appendSeparator(text);
            appendDoubleField(text, subNote.sampleBinding->m_volume);
            appendLineBreak(text);
        }
        appendNoteColorLines(text, "SC", subNote.customColors);
        appendNoteMetadataLines(text, "SM", subNote.metadata);
    }
}

/// @brief 解析一行主音符数据。
/// @param fields 包含行类型在内的八个字段。
/// @return 完整基础组件；任一字段非法时丢弃该主行。
/// @note 只还原协议数据，不分配 ECS 实体或校验目标谱面的轨道容量。
/// @note 有限负时长等业务范围问题不在此拒绝；粘贴入口仍须执行编辑约束。
/// @note 不把 HOLD、FLICK 等类型的未使用几何字段强制置零，以保留协议原值。
std::optional<NoteComponent> parseMainNoteLine(
    const std::vector<std::string_view>& fields)
{
    if ( fields.size() != 8 ) {
        return std::nullopt;
    }

    const auto type = noteTypeFromCode(fields[1]);
    // 固定列数先行检查，随后字段索引不依赖输入者提供正确长度。
    const auto timestamp = parseDoubleField(fields[2]);
    const auto duration  = parseDoubleField(fields[3]);
    const auto track     = parseIntField(fields[4]);
    const auto dtrack    = parseIntField(fields[5]);
    const auto isSubNote = parseBoolField(fields[6]);
    const auto subIndex  = parseIntField(fields[7]);
    // 检查 optional 是否有值，合法的 false、零时间和零轨道都不能当作失败。
    if ( !type || !timestamp || !duration || !track || !dtrack || !isSubNote ||
         !subIndex ) {
        return std::nullopt;
    }

    NoteComponent note;
    note.m_type       = *type;
    note.m_timestamp  = *timestamp;
    note.m_duration   = *duration;
    note.m_trackIndex = *track;
    note.m_dtrack     = *dtrack;
    note.m_isSubNote  = *isSubNote;
    // 子标志与子索引照单保存，不凭这两个字段建立父子关系或查找源实体。
    note.m_isDraft = note.m_trackIndex < 0;
    // 草稿身份从协议中的负轨道编码恢复，不依赖未传输的会话布局。
    note.m_parentPolyline = entt::null;
    // 原会话的实体句柄不能跨剪贴板使用，父实体关联留给粘贴创建流程重建。
    note.m_subIndex = *subIndex;
    // 剩余可选属性保持组件默认值；省略扩展行不会继承上一个解析条目的内容。
    return note;
}

/// @brief 解析一行子音符数据。
/// @param fields 包含类型、时间、持续时间和轨道信息的六个字段。
/// @return 子节点值快照；失败时不返回半初始化对象。
/// @note 子节点没有独立父句柄，载荷解析器负责将其挂到当前主音符。
/// @note 类型使用共同码表，这一层不证明该类型可作为目标折线的合法叶节点。
/// @note 解析成功只提供值节点，不验证它与父物件的时间先后或轨道关系。
std::optional<NoteComponent::SubNote> parseSubNoteLine(
    const std::vector<std::string_view>& fields)
{
    if ( fields.size() != 6 ) {
        return std::nullopt;
    }

    const auto type      = noteTypeFromCode(fields[1]);
    const auto timestamp = parseDoubleField(fields[2]);
    const auto duration  = parseDoubleField(fields[3]);
    const auto track     = parseIntField(fields[4]);
    const auto dtrack    = parseIntField(fields[5]);
    if ( !type || !timestamp || !duration || !track || !dtrack ) {
        return std::nullopt;
    }

    NoteComponent::SubNote subNote;
    // 独立构造节点值，不复用上一个节点对象，避免遗漏扩展行时串入旧属性。
    // 保留原始有符号轨道和持续时间，不在协议层进行吸附或位置平移。
    subNote.type       = *type;
    subNote.timestamp  = *timestamp;
    subNote.duration   = *duration;
    subNote.trackIndex = *track;
    subNote.dtrack     = *dtrack;
    return subNote;
}

/// @brief 将一行拍位数据解析到当前复制音符中。
/// @param fields 主起止拍位及两个子节点拍位列表。
/// @param item 已成功解析主行的目标音符条目。
/// @note 无效主拍位行不改变已有拍位数据；列表内部允许逐项容错。
/// @note 不要求终点大于起点，方向及范围校验留给采用拍位定位的编辑操作。
void parseBeatLine(const std::vector<std::string_view>& fields,
                   ClipboardItem&                       item)
{
    if ( fields.size() != 5 ) {
        return;
    }

    const auto startBeat = parseDoubleField(fields[1]);
    const auto endBeat   = parseDoubleField(fields[2]);
    if ( !startBeat || !endBeat ) {
        return;
    }

    item.startBeat = *startBeat;
    // 两个主拍位均有效后才覆盖目标，避免仅更新起点而留下旧终点。
    item.endBeat       = *endBeat;
    item.subStartBeats = parseNumberListField(fields[3]);
    // 两个列表独立容错，坏项可能使它们长度不同；接收方不能无条件成对索引。
    item.subEndBeats      = parseNumberListField(fields[4]);
    item.hasBeatPositions = true;
    // 重复有效 NB 行整体替换列表，不与先前行的节点拍位拼接。
    // 标志只表示该扩展行被接受，不保证子列表与子节点数量完全一致。
}

/// @brief 追加一个自动采样剪贴板条目。
/// @param text 输出协议文本。
/// @param item 已将轨道表示转换为 BGM 相对索引的条目。
/// @note 时间锚点与有符号毫秒偏移分开保存，不合并为单一浮点时间。
/// @note 不输出自动采样的协作身份，复制后身份分配由新物件创建流程负责。
/// @pre bgmLane 已正确表达复制来源的相对 BGM 轨，不从 sample.m_track 重新推断。
void appendSampleItem(std::string& text, const SampleClipboardItem& item)
{
    text.append("A");
    appendSeparator(text);
    appendDoubleField(text, item.sample.m_timestamp);
    appendSeparator(text);
    appendInt64Field(text, item.sample.m_offsetMs);
    // 偏移列保持毫秒单位，与前一列秒时间不同，不能统一按浮点秒输出。
    appendSeparator(text);
    appendUint32Field(text, item.bgmLane);
    // 使用相对 BGM 轨道，粘贴到不同玩家轨道数的谱面仍可保持所在 BGM 位置。
    appendSeparator(text);
    appendEscapedField(text, item.sample.m_audioResourceId);
    appendSeparator(text);
    appendDoubleField(text, item.sample.m_volume);
    appendSeparator(text);
    appendDoubleField(text, item.startBeat);
    // 即使无有效拍位也保留该列，布尔标志决定消费者是否采用它。
    appendSeparator(text);
    appendBoolField(text, item.hasBeatPosition);
    appendLineBreak(text);
    appendSampleMetadataLines(text, "AM", item.sample.m_metadata);
    // 元数据紧随该样本输出，不能挪到下一条样本之后而改变归属。
}

/// @brief 解析一行自动采样剪贴板数据。
/// @param fields 固定八字段的样本主行。
/// @return 有效样本条目，资源 ID 为空或音量无法表示为 float 时失败。
/// @note 此处不查询项目资源表，资源身份存在不等于目标项目已拥有该资源。
/// @note 音量只检查表示范围，不将负增益或大于一的增益钳制为界面默认范围。
/// @note 不查询音频时长，所以也不验证偏移是否落在可播放的资源区间。
std::optional<SampleClipboardItem> parseSampleItemLine(
    const std::vector<std::string_view>& fields)
{
    if ( fields.size() != 8 ) {
        return std::nullopt;
    }

    const auto timestamp       = parseDoubleField(fields[1]);
    const auto offsetMs        = parseInt64Field(fields[2]);
    const auto bgmLane         = parseUint32Field(fields[3]);
    auto       audioResourceId = decodeEscapedField(fields[4]);
    const auto volume          = parseDoubleField(fields[5]);
    const auto startBeat       = parseDoubleField(fields[6]);
    const auto hasBeatPosition = parseBoolField(fields[7]);
    // 固定列均须格式合法，包括标志为 false 时尚不参与定位的拍位列。
    // 在缩窄到 float 前限制绝对值，防止有限 double 转成无穷音量。
    if ( !timestamp || !offsetMs || !bgmLane || !audioResourceId ||
         audioResourceId->empty() || !volume || !startBeat ||
         !hasBeatPosition ||
         std::abs(*volume) >
             static_cast<double>(std::numeric_limits<float>::max()) ) {
        return std::nullopt;
    }

    SampleClipboardItem item;
    item.sample.m_timestamp = *timestamp;
    item.sample.m_offsetMs  = *offsetMs;
    item.sample.m_track     = *bgmLane;
    // 剪贴板阶段组件轨道仍是相对值，不能直接作为目标会话的绝对轨道使用。
    item.sample.m_audioResourceId = std::move(*audioResourceId);
    // 移入解码字符串后结果拥有资源标识，不依赖 fields 或原文本的寿命。
    item.sample.m_volume = static_cast<float>(*volume);
    item.bgmLane         = *bgmLane;
    item.startBeat       = *startBeat;
    item.hasBeatPosition = *hasBeatPosition;
    return item;
}

/// @brief 追加一个时间线剪贴板条目。
/// @param text 输出协议文本。
/// @param item 含原始时间点和相对定位信息的条目。
/// @note 保留时间点值本身，协议层不根据 BPM 或 SV 类型归一化效果值。
/// @note 三种时间信息同时输出，不能以其中一项非零推断其余两项可以省略。
void appendTimelineItem(std::string& text, const TimelineClipboardItem& item)
{
    text.append("T");
    appendSeparator(text);
    appendDoubleField(text, item.timeline.m_timestamp);
    appendSeparator(text);
    text.append(timingEffectCode(item.timeline.m_effect));
    appendSeparator(text);
    appendDoubleField(text, item.timeline.m_value);
    appendSeparator(text);
    appendDoubleField(text, item.relativeTime);
    // 秒偏移与拍偏移同时传输，接收方按粘贴模式选择定位依据。
    appendSeparator(text);
    appendDoubleField(text, item.relativeBeat);
    appendSeparator(text);
    appendBoolField(text, item.hasBeatPosition);
    appendLineBreak(text);
    appendTimingMetadataLines(text, "TM", item.timeline.m_metadata);
    // 扩展必须跟在所属 T 行之后，接收端没有通过时间或 ID 回查父行的步骤。
}

/// @brief 解析一行时间线剪贴板条目。
/// @param fields 包含效果码及定位信息的七个字段。
/// @return 完整时间线条目；非法效果码或非有限数值使该行失败。
/// @note 不检查相对秒偏移与相对拍偏移是否一致，两者对应的节奏环境不在载荷内。
/// @note 接受同刻不同效果，也不在单行解析中查询先前时间点的生效值。
std::optional<TimelineClipboardItem> parseTimelineItemLine(
    const std::vector<std::string_view>& fields)
{
    if ( fields.size() != 7 ) {
        return std::nullopt;
    }

    const auto timestamp       = parseDoubleField(fields[1]);
    const auto effect          = timingEffectFromCode(fields[2]);
    const auto value           = parseDoubleField(fields[3]);
    const auto relativeTime    = parseDoubleField(fields[4]);
    const auto relativeBeat    = parseDoubleField(fields[5]);
    const auto hasBeatPosition = parseBoolField(fields[6]);
    if ( !timestamp || !effect || !value || !relativeTime || !relativeBeat ||
         !hasBeatPosition ) {
        return std::nullopt;
    }

    TimelineClipboardItem item;
    // 元数据由随后的 TM 行补充；缺少扩展行不影响主时间点被接受。
    item.timeline.m_timestamp = *timestamp;
    item.timeline.m_effect    = *effect;
    item.timeline.m_value     = *value;
    item.relativeTime         = *relativeTime;
    item.relativeBeat         = *relativeBeat;
    item.hasBeatPosition      = *hasBeatPosition;
    return item;
}

/// @brief 追加紧凑协议头。
/// @param text 目标文本，调用方应在载荷行之前调用。
/// @param kind 本实现支持的载荷类别码。
/// @note 始终输出当前版本，旧版本仅作为读取兼容入口。
/// @pre kind 应来自本文件声明的类别常量，输出 helper 本身不校验未知类别。
void appendHeader(std::string& text, std::string_view kind)
{
    // 协议头独占首行，不输出 BOM 或显示标题；首字段需与解析器魔数精确匹配。
    text.append(MAGIC);
    appendSeparator(text);
    text.append(kind);
    appendLineBreak(text);
}

/// @brief 解析并验证紧凑协议头。
/// @param text 完整输入；成功时视图已移除首行。
/// @return 借用原文本的类别字段，非法头返回空值。
/// @note 头解析消费输入视图，即使失败也不恢复首行；外层 parse 使用传值视图。
/// @note 不接受前置空行或额外头列；扩展正文容错不等于放宽协议识别。
std::optional<std::string_view> parseHeader(std::string_view& text)
{
    auto line = popLine(text);
    if ( !line ) {
        return std::nullopt;
    }

    const auto fields = splitFields(*line);
    if ( fields.size() != 2 ||
         // 版本与类别分开验证，不将任意带相同前缀的文本作为协议输入。
         (fields[0] != MAGIC && fields[0] != LEGACY_MAGIC_V3 &&
          fields[0] != LEGACY_MAGIC) ) {
        return std::nullopt;
    }
    if ( fields[1] != KIND_NOTES && fields[1] != KIND_CHART_OBJECTS &&
         fields[1] != KIND_TIMELINES ) {
        return std::nullopt;
    }
    if ( fields[1] == KIND_CHART_OBJECTS && fields[0] != MAGIC ) {
        // 旧版的音符与时间线类别仍可读取，不因禁止旧版混合类别而一并拒绝。
        // 混合物件载荷是当前版本能力，不能借旧版本头启用新载荷语义。
        return std::nullopt;
    }
    return fields[1];
}

/// @brief 解析 V2、V3 或 V4 的物件采样绑定行。
/// @param fields 已按制表符拆分的 NS 或 SS 行。
/// @return 有效资源标识及音量；V2 行默认音量为 1。
/// @note 此处仅解码资源引用，不验证目标项目是否具有该音频资源。
/// @note 兼容性按实际列数判断，不依赖外层头版本选择另一套绑定解析器。
/// @note 不过滤静音绑定，显式零音量与没有绑定是不同状态。
std::optional<::MMM::AudioSampleBinding> parseSampleBindingLine(
    const std::vector<std::string_view>& fields)
{
    if ( fields.size() != 2 && fields.size() != 3 ) {
        return std::nullopt;
    }
    auto audioResourceId = decodeEscapedField(fields[1]);
    // ID 是不透明引用，不把其中的斜杠、大小写或空格按文件路径规范化。
    if ( !audioResourceId || audioResourceId->empty() ) {
        return std::nullopt;
    }

    float volume = 1.0F;
    // 老格式缺少音量列时保留单位增益，不将缺省值当作静音。
    if ( fields.size() == 3 ) {
        // 显式空音量列是坏字段，不等于旧版缺少该列，不能使用默认增益掩盖。
        const auto parsedVolume = parseDoubleField(fields[2]);
        if ( !parsedVolume ||
             std::abs(*parsedVolume) >
                 static_cast<double>(std::numeric_limits<float>::max()) ) {
            return std::nullopt;
        }
        // double 字段已验证有限性及 float 容量，缩窄转换不会引入无穷值。
        volume = static_cast<float>(*parsedVolume);
    }
    // 绑定结果为值对象，解析完成后可独立于输入行保存。
    return ::MMM::AudioSampleBinding{ std::move(*audioResourceId), volume };
}

/// @brief 解析音符或混合谱面物件载荷行。
/// @param text 不含协议头的剩余文本。
/// @param preserveCollaborationIdentity 是否允许恢复主物件和子节点的协作 ID。
/// @return 已接受条目的集合；未知行和无效可选字段不会使整个载荷失败。
/// @note 扩展行依赖最近建立的条目上下文，输入行的顺序属于协议结构。
/// @note 结果按类别分开保存，不保留音符与采样之间的交错行序。
/// @note 未知行不会终止当前上下文；该容错入口不提供逐行错误报告。
/// @warning 低频粘贴或协议读取路径，包含字符串解码与容器增长，不应逐帧解析。
ParsedClipboard parseChartObjectPayload(std::string_view text,
                                        bool preserveCollaborationIdentity)
{
    ParsedClipboard parsed;
    ClipboardItem*  currentItem = nullptr;
    // 指针只观察结果容器内的当前条目，每次追加后重新取得 back 避免扩容失效。
    SampleClipboardItem* currentSampleItem = nullptr;
    // 解析期间条目只追加不删除，当前指针的失效点限于其所属 vector 的扩容。
    // 子节点数组扩容不移动外层 ClipboardItem，但不能持久缓存某个子节点地址。

    while ( auto line = popLine(text) ) {
        if ( line->empty() ) {
            // 空行没有终止记录的含义，人工排版插入的空行不改变扩展归属。
            continue;
        }

        const auto fields = splitFields(*line);
        if ( fields.empty() ) {
            continue;
        }

        if ( fields[0] == "N" ) {
            // 每个有效 N 行都是新条目，即使时间、轨道和协作身份与此前相同。
            auto note = parseMainNoteLine(fields);
            if ( !note ) {
                // 无效主音符中断音符扩展归属，不让后续音符属性污染上一条音符。
                // 这里只清音符指针，已有采样上下文仍保留；不能视为两类统一重置。
                currentItem = nullptr;
                continue;
            }
            ClipboardItem item;
            item.note = std::move(*note);
            parsed.notes.push_back(std::move(item));
            currentItem       = &parsed.notes.back();
            currentSampleItem = nullptr;
            // 成功开始新音符后不再接受上一条样本的扩展属性。
        } else if ( fields[0] == "A" ) {
            auto item = parseSampleItemLine(fields);
            if ( !item ) {
                currentItem = nullptr;
                // 无效样本主行同时结束两类上下文，等待下一条有效主行。
                currentSampleItem = nullptr;
                continue;
            }
            parsed.samples.push_back(std::move(*item));
            currentItem       = nullptr;
            currentSampleItem = &parsed.samples.back();
        } else if ( fields[0] == "AM" && currentSampleItem ) {
            // 样本属性只能修改当前采样，不因未知行插入而自动转移给音符。
            parseSampleMetadataLine(fields,
                                    currentSampleItem->sample.m_metadata);
        } else if ( fields[0] == "NB" && currentItem ) {
            // NB 始终属于主条目，已经出现子节点也不会改变这类扩展的目标。
            parseBeatLine(fields, *currentItem);
        } else if ( fields[0] == "NI" && preserveCollaborationIdentity &&
                    currentItem && fields.size() == 2U ) {
            if ( auto identity = decodeEscapedField(fields[1]);
                 identity && !identity->empty() &&
                 identity->size() <= MAX_COLLABORATION_ID_BYTES ) {
                // 身份长度按解码后字节数限制，不按转义文本长度或字符数判断。
                // 保留开关不是唯一性校验，载荷中的重复身份仍由使用方处理。
                currentItem->note.m_collaborationId = std::move(*identity);
                // 不按该身份查找已有条目，扩展只作用于当前主行建立的对象。
            }
        } else if ( fields[0] == "NA" && currentItem && fields.size() == 2U ) {
            if ( auto annotation = decodeEscapedField(fields[1]);
                 annotation &&
                 annotation->size() <= ::MMM::MAX_NOTE_ANNOTATION_BYTES ) {
                // 空批注允许覆盖为空，超长或转义损坏的扩展不替换已有内容。
                currentItem->note.m_annotation = std::move(*annotation);
            }
        } else if ( fields[0] == "NS" && currentItem ) {
            // 坏绑定行不清除此前绑定；协议没有用空资源 ID 表达解绑的规则。
            if ( auto binding = parseSampleBindingLine(fields) ) {
                // 一行绑定是完整值替换，资源 ID 和音量不能分别沿用不同历史行。
                currentItem->note.m_sampleBinding = std::move(*binding);
            }
        } else if ( fields[0] == "NC" && currentItem ) {
            // 颜色专用行与元数据行分别保留，不在协议层决定它们在渲染中的优先级。
            if ( auto color = parseColorLine(fields) ) {
                assignColor(
                    currentItem->note.m_customColors, fields[1], *color);
            }
        } else if ( fields[0] == "NM" && currentItem ) {
            // NM 即使出现在 S 后仍指向父音符；子元数据必须使用 SM 标签。
            parseNoteMetadataLine(fields, currentItem->note.m_metadata);
        } else if ( fields[0] == "S" && currentItem ) {
            // 子节点只能附加到已接受的主音符，没有主上下文时忽略孤立子行。
            // 这里不检查主音符必须是 POLYLINE，结构合法性仍由粘贴侧约束。
            // 坏子行不清空已有节点，因此随后的子扩展仍指向最后成功追加的节点。
            if ( auto subNote = parseSubNoteLine(fields) ) {
                // 只保存节点值，不为子行分配实体或回填源 Registry 的关联句柄。
                currentItem->note.m_subNotes.push_back(std::move(*subNote));
            }
        } else if ( fields[0] == "SI" && preserveCollaborationIdentity &&
                    currentItem && !currentItem->note.m_subNotes.empty() &&
                    fields.size() == 2U ) {
            // 子节点扩展以最近成功追加的子节点为目标，且同样受身份保留开关控制。
            if ( auto identity = decodeEscapedField(fields[1]);
                 identity && !identity->empty() &&
                 identity->size() <= MAX_COLLABORATION_ID_BYTES ) {
                currentItem->note.m_subNotes.back().collaborationId =
                    std::move(*identity);
                // 节点身份与父身份独立，不通过父 ID 和数组下标在此派生。
            }
        } else if ( fields[0] == "SS" && currentItem &&
                    !currentItem->note.m_subNotes.empty() ) {
            // 绑定只更新当前子节点，不继承或覆盖根物件的采样绑定。
            if ( auto binding = parseSampleBindingLine(fields) ) {
                currentItem->note.m_subNotes.back().sampleBinding =
                    std::move(*binding);
            }
        } else if ( fields[0] == "SA" && currentItem &&
                    !currentItem->note.m_subNotes.empty() &&
                    fields.size() == 2U ) {
            // 限制按单个节点文本计算，不与父批注长度合计，也不截断多字节文本。
            if ( auto annotation = decodeEscapedField(fields[1]);
                 annotation &&
                 annotation->size() <= ::MMM::MAX_NOTE_ANNOTATION_BYTES ) {
                currentItem->note.m_subNotes.back().annotation =
                    std::move(*annotation);
            }
        } else if ( fields[0] == "SC" && currentItem &&
                    !currentItem->note.m_subNotes.empty() ) {
            // 子节点的颜色覆盖保存在子节点自身，不能合并进主音符颜色表。
            if ( auto color = parseColorLine(fields) ) {
                assignColor(currentItem->note.m_subNotes.back().customColors,
                            fields[1],
                            *color);
            }
        } else if ( fields[0] == "SM" && currentItem &&
                    !currentItem->note.m_subNotes.empty() ) {
            // 前置非空检查保护 back()；孤立子扩展不能凭空创建一个默认节点。
            parseNoteMetadataLine(fields,
                                  currentItem->note.m_subNotes.back().metadata);
        }
    }

    return parsed;
}

/// @brief 解析时间线载荷行。
/// @param text 已消费协议头后的文本视图。
/// @return 按输入顺序收集的有效时间点及其扩展属性。
/// @note 不按时间排序或合并同刻效果，保留有效主行的原始次序。
/// @note 未知行与空行不清除当前时间点；坏 T 主行才中断 TM 属性归属。
/// @note 有效头后的纯扩展或未知行载荷可返回空集合，不构造默认时间点兜底。
/// @warning 低频文本解析会逐行分配字段数组与结果条目，不用于逐帧查询。
ParsedClipboard parseTimelinePayload(std::string_view text)
{
    ParsedClipboard        parsed;
    TimelineClipboardItem* currentItem = nullptr;
    // 时间线载荷不处理 N/A 行，也不维护跨类别上下文；它们按未知行跳过。

    while ( auto line = popLine(text) ) {
        if ( line->empty() ) {
            continue;
        }

        const auto fields = splitFields(*line);
        if ( fields.empty() ) {
            continue;
        }

        if ( fields[0] == "T" ) {
            auto item = parseTimelineItemLine(fields);
            if ( !item ) {
                // 主行失败清除属性归属，随后 TM 行不能误附到此前的时间点。
                currentItem = nullptr;
                continue;
            }
            parsed.timelines.push_back(std::move(*item));
            // 发布进结果的是已通过基础校验的完整条目，失败主行不占结果位置。
            currentItem = &parsed.timelines.back();
            // 容器扩容后重新获取当前条目地址，不保存先前元素的悬空指针。
        } else if ( fields[0] == "TM" && currentItem ) {
            parseTimingMetadataLine(fields, currentItem->timeline.m_metadata);
        }
    }

    return parsed;
}
}  // namespace

/// @brief 序列化纯音符剪贴板载荷。
/// @param items 按期望输出顺序排列的条目。
/// @param preserveCollaborationIdentity 是否显式携带协作身份。
/// @return 带当前协议头的完整文本，空集合也输出有效头。
/// @note 本入口仅构造文本，不调用系统剪贴板，也不修改传入的组件快照。
/// @warning 复制或导出时的低频路径，文本大小随节点和属性数量增长。
std::string serializeNotes(const std::vector<ClipboardItem>& items,
                           bool preserveCollaborationIdentity)
{
    // 输出缓冲区只属于此次调用，构造期间不会改变已有系统剪贴板内容。
    std::string text;
    appendHeader(text, KIND_NOTES);
    for ( const auto& item : items ) {
        appendClipboardItem(text, item, preserveCollaborationIdentity);
    }
    // 不返回输入视图，调用者可独立保存或异步传递完整文本。
    return text;
}

/// @brief 将音符和自动采样编码为混合物件载荷。
/// @param notes 音符值快照。
/// @param samples 使用 BGM 相对索引的采样值快照。
/// @return 先音符后采样的完整协议文本，不进行跨类别时间排序。
/// @note 只携带资源引用，不复制音频文件；跨项目资源映射由粘贴流程负责。
/// @warning 低频复制路径，串行构造整份文本，不用于每帧交换剪贴板状态。
std::string serializeChartObjects(
    const std::vector<ClipboardItem>&       notes,
    const std::vector<SampleClipboardItem>& samples)
{
    std::string text;
    appendHeader(text, KIND_CHART_OBJECTS);
    for ( const auto& item : notes ) {
        // 混合剪贴板面向复制粘贴，不保留音符协作身份以免生成重复身份。
        appendClipboardItem(text, item, false);
    }
    for ( const auto& item : samples ) {
        // 混合输出的采样轨道已由复制层换算；此处不需要目标谱面的玩家轨数。
        appendSampleItem(text, item);
    }
    return text;
}

/// @brief 将时间线条目编码为独立的时间线载荷。
/// @param items 包含定位偏移与扩展属性的时间点集合。
/// @return 带类型头的文本，条目保持调用方顺序。
/// @note 不更改原条目的绝对时间或相对偏移，也不替目标谱面执行节奏定位。
/// @warning 低频序列化路径，元数据编码可能分配较大的文本缓冲区。
std::string serializeTimelines(const std::vector<TimelineClipboardItem>& items)
{
    // 效果值文本不依赖当前会话 BPM，即使不同项目之间传递也保持来源数据。
    std::string text;
    appendHeader(text, KIND_TIMELINES);
    for ( const auto& item : items ) {
        appendTimelineItem(text, item);
    }
    return text;
}

/// @brief 验证协议头并按载荷类型解析剪贴板文本。
/// @param text 完整输入，在调用期间保持底层存储有效。
/// @param preserveCollaborationIdentity 是否接受音符身份扩展行。
/// @return 无效协议头返回空值；有效头即使没有有效条目也返回空集合结果。
/// @note 返回结果拥有自己的数据，不继续借用输入文本。
/// @note 成功只证明协议头受支持，不代表正文完整、物件可粘贴或资源可解析。
/// @note 普通外部文本不会被当作其他格式重试，调用方可依据空值选择自身后备。
/// @note 不修改当前项目、选择集或操作栈；是否采用返回结果由编辑层决定。
/// @warning 低频完整载荷解析，耗时与行数及字符串总量相关，不应逐帧重复执行。
std::optional<ParsedClipboard> parse(std::string_view text,
                                     bool preserveCollaborationIdentity)
{
    const auto kind = parseHeader(text);
    // text 是传值视图，消费协议头不会改变调用者保存的字符串或视图位置。
    if ( !kind ) {
        // 头错误不进入正文解析，避免从任意外部文本中偶然识别出物件行。
        return std::nullopt;
    }

    if ( *kind == KIND_NOTES || *kind == KIND_CHART_OBJECTS ) {
        // 纯音符与混合物件共用行解析器，时间线则使用独立的条目上下文。
        // 因此 N 类头也可读到 A 行，不用头类别充当正文物件种类的严格白名单。
        return parseChartObjectPayload(text, preserveCollaborationIdentity);
    }
    // 时间线入口不使用协作身份保留开关，其协议条目没有对应的身份扩展行。
    // 到达此处分支时头类别已经验证，不能将未知类别作为时间线继续尝试。
    return parseTimelinePayload(text);
}

}  // namespace MMM::Logic::EditorClipboardProtocol
