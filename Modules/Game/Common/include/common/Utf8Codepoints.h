#pragma once

#include "common/FontConstants.h"
#include "common/UnicodeCodepoint.h"
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace MMM::Common
{

/// @brief 从 UTF-8 文本解码下一个 Unicode 码点。
/// @param text UTF-8 文本。
/// @param offset 输入与输出字节偏移；无效序列至少前进一个字节。
/// @return 解码码点；无效序列返回替换字符。
/// @details 输入是有界视图，嵌入的零字节仍作为有效 ASCII 码点消费。
/// 非法首字节、截断和错误后续字节仅消费首字节，便于下一轮恢复。
/// 完整序列解出的非法标量则消费整个序列，保持历史偏移语义。
/// @warning 主画布文本热路径：只读取至多四个字节，不分配内存。
[[nodiscard]] inline std::uint32_t decodeNextUtf8Codepoint(
    std::string_view text, std::size_t& offset)
{
    // 到达文本末尾返回零且不移动偏移，调用循环以 size 条件终止。
    if ( offset >= text.size() ) return 0U;

    // 使用无符号字节解析，避免平台 char 符号性改变高位掩码结果。
    const auto first = static_cast<unsigned char>(text[offset]);
    // ASCII 单字节路径直接返回，避免进入多字节状态机。
    if ( first < 0x80U ) {
        ++offset;
        return first;
    }

    std::size_t   sequenceLength = 0U;
    std::uint32_t codepoint      = 0U;
    std::uint32_t minimumValue   = 0U;
    // 首字节范围同时决定序列长度、初始有效位和防过长编码的最小值。
    // 两字节首字节排除 C0/C1，它们只能构成 ASCII 的过长编码。
    if ( first >= 0xC2U && first <= 0xDFU ) {
        sequenceLength = 2U;
        codepoint      = first & 0x1FU;
        minimumValue   = 0x80U;
        // 三字节的代理项限制在序列组装后统一检查。
    } else if ( first >= 0xE0U && first <= 0xEFU ) {
        sequenceLength = 3U;
        codepoint      = first & 0x0FU;
        minimumValue   = 0x800U;
        // 四字节只允许 F0 至 F4；更高首字节不可能落在 Unicode 范围内。
    } else if ( first >= 0xF0U && first <= 0xF4U ) {
        sequenceLength = 4U;
        codepoint      = first & 0x07U;
        minimumValue   = 0x10000U;
    } else {
        // 非法首字节只消费一个字节，使后续有效序列仍有机会恢复解析。
        ++offset;
        return UNICODE_REPLACEMENT_CODEPOINT;
    }

    // offset 已在入口验证，使用剩余长度比较避免加法越界。
    if ( text.size() - offset < sequenceLength ) {
        // 截断序列同样只前进一个字节，维持“每次至少推进一字节”的契约。
        ++offset;
        return UNICODE_REPLACEMENT_CODEPOINT;
    }
    for ( std::size_t index = 1U; index < sequenceLength; ++index ) {
        // 每个后续字节必须以二进制 10 开头，否则当前序列立即失败。
        const auto continuation =
            static_cast<unsigned char>(text[offset + index]);
        if ( (continuation & 0xC0U) != 0x80U ) {
            // 偏移只跳过首字节，异常后续字节由下一次调用重新解释。
            ++offset;
            return UNICODE_REPLACEMENT_CODEPOINT;
        }
        // 累积六位有效载荷，最多执行三次且不会超出 uint32_t。
        codepoint = (codepoint << 6U) | (continuation & 0x3FU);
    }

    // 到此每个后续字节都已验证，消费长度不因标量合法性而回退。
    offset += sequenceLength;
    // 最小值排除过长编码，标量检查排除代理项和 Unicode 上限外值。
    if ( codepoint < minimumValue || !isValidUnicodeCodepoint(codepoint) ) {
        return UNICODE_REPLACEMENT_CODEPOINT;
    }
    return codepoint;
}

/// @brief 收集 UTF-8 文本中的非 ASCII Unicode 码点。
/// @param target 接收码点的低频工作缓冲区。
/// @param text 待扫描 UTF-8 文本。
/// @warning 字体图集重载路径：允许追加并在调用方统一排序去重，禁止在每帧调用。
inline void appendNonAsciiCodepoints(std::vector<std::uint32_t>& target,
                                     std::string_view            text)
{
    // 收集器只追加，排序、去重和图集资源创建仍由原调用方负责。
    std::size_t offset = 0U;
    // 解码器保证有效输入和无效输入都推进，循环不会停留在坏字节上。
    while ( offset < text.size() ) {
        const auto codepoint = decodeNextUtf8Codepoint(text, offset);
        // ASCII 已由固定字形集覆盖，仅收集需要动态载入的非 ASCII 码点。
        if ( codepoint > ASCII_GLYPH_LAST ) {
            target.push_back(codepoint);
        }
    }
}

}  // namespace MMM::Common
