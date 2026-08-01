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
    if ( text.empty() ) return {};

#if defined(__APPLE__)
    const auto isLeadingAsciiSpace = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
               ch == '\f' || ch == '\v';
    };
    if ( text.front() == '+' || isLeadingAsciiSpace(text.front()) ) return {};

    const std::string nullTerminatedText(text);
    char*             parseEnd = nullptr;
    errno                      = 0;
    const double parsed =
        ::strtod_l(nullTerminatedText.c_str(), &parseEnd, LC_C_LOCALE);
    const std::size_t parsedLength =
        static_cast<std::size_t>(parseEnd - nullTerminatedText.c_str());
    if ( parsedLength == 0 ) return {};
    if ( errno == ERANGE ) {
        return { parsed, parsedLength, std::errc::result_out_of_range };
    }
    return { parsed, parsedLength, {} };
#else
    double     parsed = 0.0;
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
    while ( !text.empty() ) {
        const unsigned char c = static_cast<unsigned char>(text.front());
        if ( c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' &&
             c != '\v' ) {
            break;
        }
        text.remove_prefix(1);
    }
    return text;
}

inline std::string safeAt(const std::vector<std::string>& v, size_t idx,
                          const std::string& defaultVal = "")
{
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
    if ( s.empty() ) return defaultVal;
    auto text = trimLeadingAsciiSpaces(s);
    if ( text.empty() ) return defaultVal;

    int        value = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if ( result.ec == std::errc{} && result.ptr != text.data() ) {
        return value;
    }

    const auto doubleResult = parseFloatingPrefix(text);
    if ( doubleResult.error == std::errc{} && doubleResult.parsedLength != 0 &&
         std::isfinite(doubleResult.value) &&
         doubleResult.value >=
             static_cast<double>(std::numeric_limits<int>::min()) &&
         doubleResult.value <=
             static_cast<double>(std::numeric_limits<int>::max()) ) {
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
    if ( s.empty() ) return defaultVal;
    auto text = trimLeadingAsciiSpaces(s);
    if ( text.empty() ) return defaultVal;

    const auto result = parseFloatingPrefix(text);
    if ( result.error == std::errc{} && result.parsedLength != 0 ) {
        return result.value;
    }
    return defaultVal;
}

}  // namespace MMM::Internal
