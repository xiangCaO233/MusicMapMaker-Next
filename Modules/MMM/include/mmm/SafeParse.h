#pragma once
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace MMM::Internal
{

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

    double     doubleValue = 0.0;
    const auto doubleResult =
        std::from_chars(text.data(), text.data() + text.size(), doubleValue);
    if ( doubleResult.ec == std::errc{} && doubleResult.ptr != text.data() &&
         std::isfinite(doubleValue) &&
         doubleValue >= static_cast<double>(std::numeric_limits<int>::min()) &&
         doubleValue <= static_cast<double>(std::numeric_limits<int>::max()) ) {
        return static_cast<int>(doubleValue);
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

    double     value = 0.0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if ( result.ec == std::errc{} && result.ptr != text.data() ) {
        return value;
    }
    return defaultVal;
}

}  // namespace MMM::Internal
