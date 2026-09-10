#pragma once
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__APPLE__)
#    include <xlocale.h>
#endif

namespace MMM::Internal
{

/// @brief 浮点文本解析结果。
struct FloatingParseResult {
    /// @brief 解析得到的双精度值。
    double value{ 0.0 };
    /// @brief 已消费的输入字符数。
    std::size_t parsedLength{ 0 };
    /// @brief 解析错误；成功时为默认构造的 `std::errc`。
    std::errc error{ std::errc::invalid_argument };
};

/// @brief 使用固定小数点语义解析浮点文本前缀。
/// @details macOS 26 之前没有可部署的浮点 `std::from_chars`，因此在
/// Apple 平台使用系统 C locale 的 `strtod_l`，其他平台保持原有
/// `std::from_chars` 行为。
/// @param text 待解析字符串视图。
/// @return 解析值、已消费字符数和错误状态。
inline FloatingParseResult parseFloatingPrefix(std::string_view text)
{
    // 空输入没有可消费前缀，维持默认 invalid_argument 结果。
    if ( text.empty() ) return {};

#if defined(__APPLE__)
    const auto isLeadingAsciiSpace = [](char ch) {
        // 明确列出 ASCII 空白，避免区域设置改变识别范围。
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
               ch == '\f' || ch == '\v';
    };
    // 与 from_chars 对齐：不接受前导加号，也不在本层跳过空白。
    if ( text.front() == '+' || isLeadingAsciiSpace(text.front()) ) return {};

    // strtod_l 需要零结尾缓冲；该分配仅存在于文件解析低频路径。
    const std::string nullTerminatedText(text);
    char*             parseEnd = nullptr;
    errno                      = 0;
    const double parsed =
        ::strtod_l(nullTerminatedText.c_str(), &parseEnd, LC_C_LOCALE);
    const std::size_t parsedLength =
        static_cast<std::size_t>(parseEnd - nullTerminatedText.c_str());
    // 指针未前进表示没有形成任何数值前缀。
    if ( parsedLength == 0 ) return {};
    if ( errno == ERANGE ) {
        // 保留系统得到的饱和值与消费长度，同时显式报告范围错误。
        return { parsed, parsedLength, std::errc::result_out_of_range };
    }
    return { parsed, parsedLength, {} };
#else
    double parsed = 0.0;
    // 非 Apple 平台直接复用无区域设置、无分配的 from_chars 语义。
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    return { parsed,
             static_cast<std::size_t>(result.ptr - text.data()),
             result.ec };
#endif
}

/// @brief 去掉字符串视图开头的 ASCII 空白字符。
/// @param text 待处理字符串视图。
/// @return 去掉开头空白后的字符串视图。
inline std::string_view trimLeadingAsciiSpaces(std::string_view text)
{
    // 仅移动视图起点，原字符串的所有权和内容都保持不变。
    while ( !text.empty() ) {
        const unsigned char c = static_cast<unsigned char>(text.front());
        if ( c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' &&
             c != '\v' ) {
            break;
        }
        // 单字节 ASCII 判断允许安全地逐字符缩短前缀。
        text.remove_prefix(1);
    }
    return text;
}

/// @brief 安全读取字符串字段。
/// @param v 已按格式分隔的字段数组。
/// @param idx 目标字段索引。
/// @param defaultVal 越界时返回的默认文本。
/// @return 指定字段的副本，索引越界时返回默认文本。
inline std::string safeAt(const std::vector<std::string>& v, size_t idx,
                          const std::string& defaultVal = "")
{
    // 格式导入统一走此边界，避免截断行触发 vector 越界访问。
    if ( idx >= v.size() ) return defaultVal;
    return v[idx];
}

/// @brief 安全解析整数，失败时返回默认值。
/// @details 兼容旧 `std::stoi` 行为：允许前导空白和数字后的非数字尾巴。
/// @param s 待解析字符串。
/// @param defaultVal 解析失败时的默认值。
/// @return 解析得到的整数或默认值。
inline int safeStoi(const std::string& s, int defaultVal = 0)
{
    // 空字段与纯空白字段都按格式缺省值处理。
    if ( s.empty() ) return defaultVal;
    auto text = trimLeadingAsciiSpaces(s);
    if ( text.empty() ) return defaultVal;

    int value = 0;
    // 先尝试严格整数前缀，覆盖绝大多数格式字段且无需浮点转换。
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if ( result.ec == std::errc{} && result.ptr != text.data() ) {
        return value;
    }

    // 兼容历史 stoi 对 `1.0` 等来源数据的接受行为，再尝试浮点前缀。
    const auto doubleResult = parseFloatingPrefix(text);
    if ( doubleResult.error == std::errc{} && doubleResult.parsedLength != 0 &&
         std::isfinite(doubleResult.value) &&
         doubleResult.value >=
             static_cast<double>(std::numeric_limits<int>::min()) &&
         doubleResult.value <=
             static_cast<double>(std::numeric_limits<int>::max()) ) {
        // 范围预检后截断小数部分，与旧 static_cast 行为一致。
        return static_cast<int>(doubleResult.value);
    }
    return defaultVal;
}

/// @brief 安全解析浮点数，失败时返回默认值。
/// @details 兼容旧 `std::stod` 行为：允许前导空白和数字后的非数字尾巴。
/// @param s 待解析字符串。
/// @param defaultVal 解析失败时的默认值。
/// @return 解析得到的浮点数或默认值。
inline double safeStod(const std::string& s, double defaultVal = 0.0)
{
    // 调用方给出的默认值同时覆盖空字段、纯空白与解析错误。
    if ( s.empty() ) return defaultVal;
    auto text = trimLeadingAsciiSpaces(s);
    if ( text.empty() ) return defaultVal;

    // 只要求存在合法前缀，尾随格式文本按旧 stod 兼容语义忽略。
    const auto result = parseFloatingPrefix(text);
    if ( result.error == std::errc{} && result.parsedLength != 0 ) {
        return result.value;
    }
    return defaultVal;
}

}  // namespace MMM::Internal
