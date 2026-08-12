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
constexpr std::string_view KIND_NOTES = "N";

/// @brief 混合谱面物件剪贴板条目的载荷类型码。
constexpr std::string_view KIND_CHART_OBJECTS = "C";

/// @brief 时间线剪贴板条目的载荷类型码。
constexpr std::string_view KIND_TIMELINES = "T";

/// @brief 追加一个制表符分隔符。
void appendSeparator(std::string& text)
{
    text.push_back('\t');
}

/// @brief 追加一个换行符。
void appendLineBreak(std::string& text)
{
    text.push_back('\n');
}

/// @brief 将一个十六进制半字节转换为可打印字符。
char hexDigit(unsigned int value)
{
    return static_cast<char>(value < 10U ? ('0' + value) : ('A' + value - 10U));
}

/// @brief 将一个可打印十六进制字符转换为半字节。
std::optional<unsigned int> hexValue(char ch)
{
    if ( ch >= '0' && ch <= '9' ) {
        return static_cast<unsigned int>(ch - '0');
    }
    if ( ch >= 'A' && ch <= 'F' ) {
        return static_cast<unsigned int>(ch - 'A' + 10);
    }
    if ( ch >= 'a' && ch <= 'f' ) {
        return static_cast<unsigned int>(ch - 'a' + 10);
    }
    return std::nullopt;
}

/// @brief 使用百分号转义追加可安全包含制表符和换行的元数据字符串。
void appendEscapedField(std::string& text, std::string_view value)
{
    for ( unsigned char ch : value ) {
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
std::optional<std::string> decodeEscapedField(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for ( std::size_t index = 0; index < value.size(); ++index ) {
        const char ch = value[index];
        if ( ch != '%' ) {
            result.push_back(ch);
            continue;
        }
        if ( index + 2 >= value.size() ) {
            return std::nullopt;
        }
        const auto high = hexValue(value[index + 1]);
        const auto low  = hexValue(value[index + 2]);
        if ( !high || !low ) {
            return std::nullopt;
        }
        result.push_back(static_cast<char>(((*high) << 4U) | (*low)));
        index += 2;
    }
    return result;
}

/// @brief 追加一个整数字段。
void appendIntField(std::string& text, int value)
{
    text += fmt::format("{}", value);
}

/// @brief 追加一个有符号 64 位整数字段。
void appendInt64Field(std::string& text, std::int64_t value)
{
    text += fmt::format("{}", value);
}

/// @brief 追加一个无符号 32 位整数字段。
void appendUint32Field(std::string& text, std::uint32_t value)
{
    text += fmt::format("{}", value);
}

/// @brief 以 0 或 1 追加一个布尔字段。
void appendBoolField(std::string& text, bool value)
{
    text.push_back(value ? '1' : '0');
}

/// @brief 追加一个紧凑浮点数字段。
void appendDoubleField(std::string& text, double value)
{
    if ( !std::isfinite(value) ) {
        text.push_back('0');
        return;
    }
    text += fmt::format("{:.17g}", value);
}

/// @brief 解析一个整数字段。
std::optional<int> parseIntField(std::string_view field)
{
    int value = 0;
    auto [ptr, ec] =
        std::from_chars(field.data(), field.data() + field.size(), value);
    if ( ec != std::errc{} || ptr != field.data() + field.size() ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 解析一个有符号 64 位整数字段。
std::optional<std::int64_t> parseInt64Field(std::string_view field)
{
    std::int64_t value = 0;
    auto [ptr, ec] =
        std::from_chars(field.data(), field.data() + field.size(), value);
    if ( ec != std::errc{} || ptr != field.data() + field.size() ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 解析一个无符号 32 位整数字段。
std::optional<std::uint32_t> parseUint32Field(std::string_view field)
{
    std::uint32_t value = 0;
    auto [ptr, ec] =
        std::from_chars(field.data(), field.data() + field.size(), value);
    if ( ec != std::errc{} || ptr != field.data() + field.size() ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 解析一个布尔字段。
std::optional<bool> parseBoolField(std::string_view field)
{
    if ( field == "0" ) return false;
    if ( field == "1" ) return true;
    return std::nullopt;
}

/// @brief 解析一个有限浮点数字段。
std::optional<double> parseDoubleField(std::string_view field)
{
    const auto result = Internal::parseFloatingPrefix(field);
    if ( result.error != std::errc{} || result.parsedLength != field.size() ||
         !std::isfinite(result.value) ) {
        return std::nullopt;
    }
    return result.value;
}

/// @brief 拆分一行以制表符分隔的协议文本。
std::vector<std::string_view> splitFields(std::string_view line)
{
    std::vector<std::string_view> fields;
    std::size_t                   start = 0;
    while ( start <= line.size() ) {
        const std::size_t end = line.find('\t', start);
        if ( end == std::string_view::npos ) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, end - start));
        start = end + 1;
    }
    return fields;
}

/// @brief 从字符串视图读取并移除一行。
std::optional<std::string_view> popLine(std::string_view& text)
{
    if ( text.empty() ) {
        return std::nullopt;
    }

    const std::size_t end = text.find('\n');
    std::string_view  line;
    if ( end == std::string_view::npos ) {
        line = text;
        text = {};
    } else {
        line = text.substr(0, end);
        text.remove_prefix(end + 1);
    }
    if ( line.ends_with('\r') ) {
        line.remove_suffix(1);
    }
    return line;
}

/// @brief 将音符类型转换为单字符协议码。
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
std::optional<::MMM::NoteType> noteTypeFromCode(std::string_view code)
{
    if ( code == "n" ) return ::MMM::NoteType::NOTE;
    if ( code == "h" ) return ::MMM::NoteType::HOLD;
    if ( code == "f" ) return ::MMM::NoteType::FLICK;
    if ( code == "p" ) return ::MMM::NoteType::POLYLINE;
    return std::nullopt;
}

/// @brief 将时间线效果转换为单字符协议码。
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
std::optional<::MMM::TimingEffect> timingEffectFromCode(std::string_view code)
{
    if ( code == "b" ) return ::MMM::TimingEffect::BPM;
    if ( code == "s" ) return ::MMM::TimingEffect::SCROLL;
    if ( code == "j" ) return ::MMM::TimingEffect::JUMP;
    if ( code == "h" ) return ::MMM::TimingEffect::HS;
    return std::nullopt;
}

/// @brief 将音符元数据来源转换为紧凑协议码。
std::string_view noteMetadataSourceCode(::MMM::NoteMetadataType type)
{
    switch ( type ) {
    case ::MMM::NoteMetadataType::OSU: return "o";
    case ::MMM::NoteMetadataType::MALODY: return "ma";
    case ::MMM::NoteMetadataType::RM: return "r";
    case ::MMM::NoteMetadataType::MMM: return "m";
    }
    return {};
}

/// @brief 将紧凑协议码转换为音符元数据来源。
std::optional<::MMM::NoteMetadataType> noteMetadataSourceFromCode(
    std::string_view code)
{
    if ( code == "o" ) return ::MMM::NoteMetadataType::OSU;
    if ( code == "ma" ) return ::MMM::NoteMetadataType::MALODY;
    if ( code == "r" ) return ::MMM::NoteMetadataType::RM;
    if ( code == "m" ) return ::MMM::NoteMetadataType::MMM;
    return std::nullopt;
}

/// @brief 将时间线元数据来源转换为紧凑协议码。
std::string_view timingMetadataSourceCode(::MMM::TimingMetadataType type)
{
    switch ( type ) {
    case ::MMM::TimingMetadataType::OSU: return "o";
    case ::MMM::TimingMetadataType::RM: return "r";
    case ::MMM::TimingMetadataType::MALODY: return "ma";
    }
    return {};
}

/// @brief 将紧凑协议码转换为时间线元数据来源。
std::optional<::MMM::TimingMetadataType> timingMetadataSourceFromCode(
    std::string_view code)
{
    if ( code == "o" ) return ::MMM::TimingMetadataType::OSU;
    if ( code == "r" ) return ::MMM::TimingMetadataType::RM;
    if ( code == "ma" ) return ::MMM::TimingMetadataType::MALODY;
    return std::nullopt;
}

/// @brief 将自动采样元数据来源转换为紧凑协议码。
std::string_view sampleMetadataSourceCode(::MMM::SampleMetadataType type)
{
    switch ( type ) {
    case ::MMM::SampleMetadataType::MALODY: return "ma";
    case ::MMM::SampleMetadataType::MMM: return "m";
    }
    return {};
}

/// @brief 将紧凑协议码转换为自动采样元数据来源。
std::optional<::MMM::SampleMetadataType> sampleMetadataSourceFromCode(
    std::string_view code)
{
    if ( code == "ma" ) return ::MMM::SampleMetadataType::MALODY;
    if ( code == "m" ) return ::MMM::SampleMetadataType::MMM;
    return std::nullopt;
}

/// @brief 颜色存在时追加一行颜色覆盖。
void appendColorLine(std::string& text, std::string_view prefix,
                     std::string_view                code,
                     const std::optional<glm::vec4>& color)
{
    if ( !color ) {
        return;
    }

    text.append(prefix);
    appendSeparator(text);
    text.append(code);
    appendSeparator(text);
    appendDoubleField(text, color->r);
    appendSeparator(text);
    appendDoubleField(text, color->g);
    appendSeparator(text);
    appendDoubleField(text, color->b);
    appendSeparator(text);
    appendDoubleField(text, color->a);
    appendLineBreak(text);
}

/// @brief 追加音符颜色覆盖行。
void appendNoteColorLines(std::string& text, std::string_view prefix,
                          const NoteColorOverrides& colors)
{
    appendColorLine(text, prefix, "t", colors.tap);
    appendColorLine(text, prefix, "h", colors.head);
    appendColorLine(text, prefix, "b", colors.hold);
    appendColorLine(text, prefix, "e", colors.end);
    appendColorLine(text, prefix, "f", colors.flickArrow);
    appendColorLine(text, prefix, "n", colors.node);
}

/// @brief 解析一行颜色覆盖。
std::optional<glm::vec4> parseColorLine(
    const std::vector<std::string_view>& fields)
{
    if ( fields.size() != 6 ) {
        return std::nullopt;
    }

    const auto r = parseDoubleField(fields[2]);
    const auto g = parseDoubleField(fields[3]);
    const auto b = parseDoubleField(fields[4]);
    const auto a = parseDoubleField(fields[5]);
    if ( !r || !g || !b || !a ) {
        return std::nullopt;
    }

    return glm::vec4{
        std::clamp(static_cast<float>(*r), 0.0F, 1.0F),
        std::clamp(static_cast<float>(*g), 0.0F, 1.0F),
        std::clamp(static_cast<float>(*b), 0.0F, 1.0F),
        std::clamp(static_cast<float>(*a), 0.0F, 1.0F),
    };
}

/// @brief 将解析出的颜色覆盖写入目标颜色集合。
void assignColor(NoteColorOverrides& colors, std::string_view code,
                 const glm::vec4& color)
{
    if ( code == "t" ) {
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
void appendNoteMetadataLines(std::string& text, std::string_view prefix,
                             const ::MMM::NoteMetadata& metadata)
{
    for ( const auto& [source, properties] : metadata.note_properties ) {
        const auto sourceCode = noteMetadataSourceCode(source);
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

/// @brief 追加时间线元数据属性行。
void appendTimingMetadataLines(std::string& text, std::string_view prefix,
                               const ::MMM::TimingMetadata& metadata)
{
    for ( const auto& [source, properties] : metadata.timing_properties ) {
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
void appendSampleMetadataLines(std::string& text, std::string_view prefix,
                               const ::MMM::SampleMetadata& metadata)
{
    for ( const auto& [source, properties] : metadata.sample_properties ) {
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
void parseNoteMetadataLine(const std::vector<std::string_view>& fields,
                           ::MMM::NoteMetadata&                 metadata)
{
    if ( fields.size() != 4 ) {
        return;
    }

    const auto source = noteMetadataSourceFromCode(fields[1]);
    const auto key    = decodeEscapedField(fields[2]);
    const auto value  = decodeEscapedField(fields[3]);
    if ( !source || !key || !value ) {
        return;
    }
    metadata.note_properties[*source][*key] = *value;
}

/// @brief 将一行时间线元数据属性解析到元数据容器中。
void parseTimingMetadataLine(const std::vector<std::string_view>& fields,
                             ::MMM::TimingMetadata&               metadata)
{
    if ( fields.size() != 4 ) {
        return;
    }

    const auto source = timingMetadataSourceFromCode(fields[1]);
    const auto key    = decodeEscapedField(fields[2]);
    const auto value  = decodeEscapedField(fields[3]);
    if ( !source || !key || !value ) {
        return;
    }
    metadata.timing_properties[*source][*key] = *value;
}

/// @brief 将一行自动采样元数据属性解析到元数据容器中。
void parseSampleMetadataLine(const std::vector<std::string_view>& fields,
                             ::MMM::SampleMetadata&               metadata)
{
    if ( fields.size() != 4 ) {
        return;
    }

    const auto source = sampleMetadataSourceFromCode(fields[1]);
    const auto key    = decodeEscapedField(fields[2]);
    const auto value  = decodeEscapedField(fields[3]);
    if ( !source || !key || !value ) {
        return;
    }
    metadata.sample_properties[*source][*key] = *value;
}

/// @brief 追加一个数字列表字段。
void appendNumberListField(std::string& text, const std::vector<double>& values)
{
    for ( std::size_t index = 0; index < values.size(); ++index ) {
        if ( index > 0 ) {
            text.push_back(',');
        }
        appendDoubleField(text, values[index]);
    }
}

/// @brief 解析一个逗号分隔的数字列表字段。
std::vector<double> parseNumberListField(std::string_view field)
{
    std::vector<double> values;
    std::size_t         start = 0;
    while ( start <= field.size() ) {
        const std::size_t end   = field.find(',', start);
        const auto        token = field.substr(
            start,
            end == std::string_view::npos ? field.size() - start : end - start);
        if ( !token.empty() ) {
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
void appendMainNoteLine(std::string& text, const NoteComponent& note)
{
    text.append("N");
    appendSeparator(text);
    text.append(noteTypeCode(note.m_type));
    appendSeparator(text);
    appendDoubleField(text, note.m_timestamp);
    appendSeparator(text);
    appendDoubleField(text, note.m_duration);
    appendSeparator(text);
    appendIntField(text, note.m_trackIndex);
    appendSeparator(text);
    appendIntField(text, note.m_dtrack);
    appendSeparator(text);
    appendBoolField(text, note.m_isSubNote);
    appendSeparator(text);
    appendIntField(text, note.m_subIndex);
    appendLineBreak(text);
}

/// @brief 追加一行子音符数据。
void appendSubNoteLine(std::string& text, const NoteComponent::SubNote& subNote)
{
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
    appendLineBreak(text);
}

/// @brief 为一个复制音符追加可选拍位数据。
void appendBeatLine(std::string& text, const ClipboardItem& item)
{
    if ( !item.hasBeatPositions ) {
        return;
    }

    text.append("NB");
    appendSeparator(text);
    appendDoubleField(text, item.startBeat);
    appendSeparator(text);
    appendDoubleField(text, item.endBeat);
    appendSeparator(text);
    appendNumberListField(text, item.subStartBeats);
    appendSeparator(text);
    appendNumberListField(text, item.subEndBeats);
    appendLineBreak(text);
}

/// @brief 追加一个复制音符及其辅助数据行。
void appendClipboardItem(std::string& text, const ClipboardItem& item)
{
    appendMainNoteLine(text, item.note);
    if ( !item.note.m_annotation.empty() ) {
        text.append("NA");
        appendSeparator(text);
        appendEscapedField(text, item.note.m_annotation);
        appendLineBreak(text);
    }
    if ( item.note.m_sampleBinding &&
         !item.note.m_sampleBinding->m_audioResourceId.empty() ) {
        text.append("NS");
        appendSeparator(text);
        appendEscapedField(text, item.note.m_sampleBinding->m_audioResourceId);
        appendSeparator(text);
        appendDoubleField(text, item.note.m_sampleBinding->m_volume);
        appendLineBreak(text);
    }
    appendBeatLine(text, item);
    appendNoteColorLines(text, "NC", item.note.m_customColors);
    appendNoteMetadataLines(text, "NM", item.note.m_metadata);

    for ( const auto& subNote : item.note.m_subNotes ) {
        appendSubNoteLine(text, subNote);
        if ( !subNote.annotation.empty() ) {
            text.append("SA");
            appendSeparator(text);
            appendEscapedField(text, subNote.annotation);
            appendLineBreak(text);
        }
        if ( subNote.sampleBinding &&
             !subNote.sampleBinding->m_audioResourceId.empty() ) {
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
std::optional<NoteComponent> parseMainNoteLine(
    const std::vector<std::string_view>& fields)
{
    if ( fields.size() != 8 ) {
        return std::nullopt;
    }

    const auto type      = noteTypeFromCode(fields[1]);
    const auto timestamp = parseDoubleField(fields[2]);
    const auto duration  = parseDoubleField(fields[3]);
    const auto track     = parseIntField(fields[4]);
    const auto dtrack    = parseIntField(fields[5]);
    const auto isSubNote = parseBoolField(fields[6]);
    const auto subIndex  = parseIntField(fields[7]);
    if ( !type || !timestamp || !duration || !track || !dtrack || !isSubNote ||
         !subIndex ) {
        return std::nullopt;
    }

    NoteComponent note;
    note.m_type           = *type;
    note.m_timestamp      = *timestamp;
    note.m_duration       = *duration;
    note.m_trackIndex     = *track;
    note.m_dtrack         = *dtrack;
    note.m_isSubNote      = *isSubNote;
    note.m_parentPolyline = entt::null;
    note.m_subIndex       = *subIndex;
    return note;
}

/// @brief 解析一行子音符数据。
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
    subNote.type       = *type;
    subNote.timestamp  = *timestamp;
    subNote.duration   = *duration;
    subNote.trackIndex = *track;
    subNote.dtrack     = *dtrack;
    return subNote;
}

/// @brief 将一行拍位数据解析到当前复制音符中。
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

    item.startBeat        = *startBeat;
    item.endBeat          = *endBeat;
    item.subStartBeats    = parseNumberListField(fields[3]);
    item.subEndBeats      = parseNumberListField(fields[4]);
    item.hasBeatPositions = true;
}

/// @brief 追加一个自动采样剪贴板条目。
void appendSampleItem(std::string& text, const SampleClipboardItem& item)
{
    text.append("A");
    appendSeparator(text);
    appendDoubleField(text, item.sample.m_timestamp);
    appendSeparator(text);
    appendInt64Field(text, item.sample.m_offsetMs);
    appendSeparator(text);
    appendUint32Field(text, item.bgmLane);
    appendSeparator(text);
    appendEscapedField(text, item.sample.m_audioResourceId);
    appendSeparator(text);
    appendDoubleField(text, item.sample.m_volume);
    appendSeparator(text);
    appendDoubleField(text, item.startBeat);
    appendSeparator(text);
    appendBoolField(text, item.hasBeatPosition);
    appendLineBreak(text);
    appendSampleMetadataLines(text, "AM", item.sample.m_metadata);
}

/// @brief 解析一行自动采样剪贴板数据。
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
    if ( !timestamp || !offsetMs || !bgmLane || !audioResourceId ||
         audioResourceId->empty() || !volume || !startBeat ||
         !hasBeatPosition ||
         std::abs(*volume) >
             static_cast<double>(std::numeric_limits<float>::max()) ) {
        return std::nullopt;
    }

    SampleClipboardItem item;
    item.sample.m_timestamp       = *timestamp;
    item.sample.m_offsetMs        = *offsetMs;
    item.sample.m_track           = *bgmLane;
    item.sample.m_audioResourceId = std::move(*audioResourceId);
    item.sample.m_volume          = static_cast<float>(*volume);
    item.bgmLane                  = *bgmLane;
    item.startBeat                = *startBeat;
    item.hasBeatPosition          = *hasBeatPosition;
    return item;
}

/// @brief 追加一个时间线剪贴板条目。
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
    appendSeparator(text);
    appendDoubleField(text, item.relativeBeat);
    appendSeparator(text);
    appendBoolField(text, item.hasBeatPosition);
    appendLineBreak(text);
    appendTimingMetadataLines(text, "TM", item.timeline.m_metadata);
}

/// @brief 解析一行时间线剪贴板条目。
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
    item.timeline.m_timestamp = *timestamp;
    item.timeline.m_effect    = *effect;
    item.timeline.m_value     = *value;
    item.relativeTime         = *relativeTime;
    item.relativeBeat         = *relativeBeat;
    item.hasBeatPosition      = *hasBeatPosition;
    return item;
}

/// @brief 追加紧凑协议头。
void appendHeader(std::string& text, std::string_view kind)
{
    text.append(MAGIC);
    appendSeparator(text);
    text.append(kind);
    appendLineBreak(text);
}

/// @brief 解析并验证紧凑协议头。
std::optional<std::string_view> parseHeader(std::string_view& text)
{
    auto line = popLine(text);
    if ( !line ) {
        return std::nullopt;
    }

    const auto fields = splitFields(*line);
    if ( fields.size() != 2 ||
         (fields[0] != MAGIC && fields[0] != LEGACY_MAGIC_V3 &&
          fields[0] != LEGACY_MAGIC) ) {
        return std::nullopt;
    }
    if ( fields[1] != KIND_NOTES && fields[1] != KIND_CHART_OBJECTS &&
         fields[1] != KIND_TIMELINES ) {
        return std::nullopt;
    }
    if ( fields[1] == KIND_CHART_OBJECTS && fields[0] != MAGIC ) {
        return std::nullopt;
    }
    return fields[1];
}

/// @brief 解析 V2、V3 或 V4 的物件采样绑定行。
/// @param fields 已按制表符拆分的 NS 或 SS 行。
/// @return 有效资源标识及音量；V2 行默认音量为 1。
std::optional<::MMM::AudioSampleBinding> parseSampleBindingLine(
    const std::vector<std::string_view>& fields)
{
    if ( fields.size() != 2 && fields.size() != 3 ) {
        return std::nullopt;
    }
    auto audioResourceId = decodeEscapedField(fields[1]);
    if ( !audioResourceId || audioResourceId->empty() ) {
        return std::nullopt;
    }

    float volume = 1.0F;
    if ( fields.size() == 3 ) {
        const auto parsedVolume = parseDoubleField(fields[2]);
        if ( !parsedVolume ||
             std::abs(*parsedVolume) >
                 static_cast<double>(std::numeric_limits<float>::max()) ) {
            return std::nullopt;
        }
        volume = static_cast<float>(*parsedVolume);
    }
    return ::MMM::AudioSampleBinding{ std::move(*audioResourceId), volume };
}

/// @brief 解析音符或混合谱面物件载荷行。
ParsedClipboard parseChartObjectPayload(std::string_view text)
{
    ParsedClipboard      parsed;
    ClipboardItem*       currentItem       = nullptr;
    SampleClipboardItem* currentSampleItem = nullptr;

    while ( auto line = popLine(text) ) {
        if ( line->empty() ) {
            continue;
        }

        const auto fields = splitFields(*line);
        if ( fields.empty() ) {
            continue;
        }

        if ( fields[0] == "N" ) {
            auto note = parseMainNoteLine(fields);
            if ( !note ) {
                currentItem = nullptr;
                continue;
            }
            ClipboardItem item;
            item.note = std::move(*note);
            parsed.notes.push_back(std::move(item));
            currentItem       = &parsed.notes.back();
            currentSampleItem = nullptr;
        } else if ( fields[0] == "A" ) {
            auto item = parseSampleItemLine(fields);
            if ( !item ) {
                currentItem       = nullptr;
                currentSampleItem = nullptr;
                continue;
            }
            parsed.samples.push_back(std::move(*item));
            currentItem       = nullptr;
            currentSampleItem = &parsed.samples.back();
        } else if ( fields[0] == "AM" && currentSampleItem ) {
            parseSampleMetadataLine(fields,
                                    currentSampleItem->sample.m_metadata);
        } else if ( fields[0] == "NB" && currentItem ) {
            parseBeatLine(fields, *currentItem);
        } else if ( fields[0] == "NA" && currentItem && fields.size() == 2U ) {
            if ( auto annotation = decodeEscapedField(fields[1]);
                 annotation &&
                 annotation->size() <= ::MMM::MAX_NOTE_ANNOTATION_BYTES ) {
                currentItem->note.m_annotation = std::move(*annotation);
            }
        } else if ( fields[0] == "NS" && currentItem ) {
            if ( auto binding = parseSampleBindingLine(fields) ) {
                currentItem->note.m_sampleBinding = std::move(*binding);
            }
        } else if ( fields[0] == "NC" && currentItem ) {
            if ( auto color = parseColorLine(fields) ) {
                assignColor(
                    currentItem->note.m_customColors, fields[1], *color);
            }
        } else if ( fields[0] == "NM" && currentItem ) {
            parseNoteMetadataLine(fields, currentItem->note.m_metadata);
        } else if ( fields[0] == "S" && currentItem ) {
            if ( auto subNote = parseSubNoteLine(fields) ) {
                currentItem->note.m_subNotes.push_back(std::move(*subNote));
            }
        } else if ( fields[0] == "SS" && currentItem &&
                    !currentItem->note.m_subNotes.empty() ) {
            if ( auto binding = parseSampleBindingLine(fields) ) {
                currentItem->note.m_subNotes.back().sampleBinding =
                    std::move(*binding);
            }
        } else if ( fields[0] == "SA" && currentItem &&
                    !currentItem->note.m_subNotes.empty() &&
                    fields.size() == 2U ) {
            if ( auto annotation = decodeEscapedField(fields[1]);
                 annotation &&
                 annotation->size() <= ::MMM::MAX_NOTE_ANNOTATION_BYTES ) {
                currentItem->note.m_subNotes.back().annotation =
                    std::move(*annotation);
            }
        } else if ( fields[0] == "SC" && currentItem &&
                    !currentItem->note.m_subNotes.empty() ) {
            if ( auto color = parseColorLine(fields) ) {
                assignColor(currentItem->note.m_subNotes.back().customColors,
                            fields[1],
                            *color);
            }
        } else if ( fields[0] == "SM" && currentItem &&
                    !currentItem->note.m_subNotes.empty() ) {
            parseNoteMetadataLine(fields,
                                  currentItem->note.m_subNotes.back().metadata);
        }
    }

    return parsed;
}

/// @brief 解析时间线载荷行。
ParsedClipboard parseTimelinePayload(std::string_view text)
{
    ParsedClipboard        parsed;
    TimelineClipboardItem* currentItem = nullptr;

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
                currentItem = nullptr;
                continue;
            }
            parsed.timelines.push_back(std::move(*item));
            currentItem = &parsed.timelines.back();
        } else if ( fields[0] == "TM" && currentItem ) {
            parseTimingMetadataLine(fields, currentItem->timeline.m_metadata);
        }
    }

    return parsed;
}
}  // namespace

std::string serializeNotes(const std::vector<ClipboardItem>& items)
{
    std::string text;
    appendHeader(text, KIND_NOTES);
    for ( const auto& item : items ) {
        appendClipboardItem(text, item);
    }
    return text;
}

std::string serializeChartObjects(
    const std::vector<ClipboardItem>&       notes,
    const std::vector<SampleClipboardItem>& samples)
{
    std::string text;
    appendHeader(text, KIND_CHART_OBJECTS);
    for ( const auto& item : notes ) {
        appendClipboardItem(text, item);
    }
    for ( const auto& item : samples ) {
        appendSampleItem(text, item);
    }
    return text;
}

std::string serializeTimelines(const std::vector<TimelineClipboardItem>& items)
{
    std::string text;
    appendHeader(text, KIND_TIMELINES);
    for ( const auto& item : items ) {
        appendTimelineItem(text, item);
    }
    return text;
}

std::optional<ParsedClipboard> parse(std::string_view text)
{
    const auto kind = parseHeader(text);
    if ( !kind ) {
        return std::nullopt;
    }

    if ( *kind == KIND_NOTES || *kind == KIND_CHART_OBJECTS ) {
        return parseChartObjectPayload(text);
    }
    return parseTimelinePayload(text);
}

}  // namespace MMM::Logic::EditorClipboardProtocol
