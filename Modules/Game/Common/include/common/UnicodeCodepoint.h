#pragma once

#include <cstdint>

namespace MMM::Common
{

/// @brief 判断码点是否属于合法 Unicode 标量值。
/// @param codepoint 待判断码点。
/// @return 合法且不属于代理项范围时返回 true。
/// @warning 渲染与解码热路径：仅比较整数范围，不分配内存。
[[nodiscard]] inline constexpr bool isValidUnicodeCodepoint(
    std::uint32_t codepoint)
{
    // Unicode 上限以外及 UTF-16 代理项范围都不是合法标量值。
    return codepoint <= 0x10FFFFU &&
           !(codepoint >= 0xD800U && codepoint <= 0xDFFFU);
}

}  // namespace MMM::Common
