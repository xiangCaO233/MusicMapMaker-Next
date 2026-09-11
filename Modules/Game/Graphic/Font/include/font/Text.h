#pragma once

#include <cstdint>
#include <string_view>

namespace MMM
{
namespace Text
{

/// @brief 按 UTF-8 前导字节宽度遍历文本中的 Unicode 码点。
/// @tparam Callback 接受单个 `uint32_t` 码点的可调用对象类型。
/// @param utf8Text 待遍历的 UTF-8 字节序列。
/// @param callback 对每个已解码码点同步调用一次的回调。
/// @note 本函数是字体渲染使用的轻量、宽容解码器；截断序列会终止遍历，
///       非法前导字节会被跳过，但不会执行完整的 UTF-8 规范化校验。
/// @note 已识别宽度的序列不会额外验证所有续字节，也不会拒绝过长编码；
///       需要严格输入验证的持久化或协议边界应使用专用 UTF-8 校验器。
/// @warning 回调在当前调用栈同步执行，不得保留指向 `utf8Text` 的临时指针。
inline void forEachCodepoint(std::string_view utf8Text, auto callback)
{
    // 使用半开区间保存字节边界，避免为非空终止的 string_view 读取哨兵字节。
    const char* ptr = utf8Text.data();
    const char* end = ptr + utf8Text.size();

    while ( ptr < end ) {
        // 每轮至少推进一个字节，非法输入也不会让遍历停留在原位置。
        uint32_t      codepoint = 0;
        unsigned char c         = static_cast<unsigned char>(*ptr);

        if ( c < 0x80 ) {
            // ASCII 与 UTF-8 共享单字节编码，可以直接提升为码点。
            codepoint = c;
            ptr++;
        } else if ( (c & 0xE0) == 0xC0 ) {
            // 在读取续字节前先拒绝被缓冲区边界截断的双字节序列。
            if ( ptr + 1 >= end ) break;
            codepoint = ((c & 0x1F) << 6) | (ptr[1] & 0x3F);
            ptr += 2;
        } else if ( (c & 0xF0) == 0xE0 ) {
            // 常见 CJK 字符使用三字节编码；截断时不产生不完整码点。
            if ( ptr + 2 >= end ) break;
            codepoint =
                ((c & 0x0F) << 12) | ((ptr[1] & 0x3F) << 6) | (ptr[2] & 0x3F);
            ptr += 3;
        } else if ( (c & 0xF8) == 0xF0 ) {
            // 补充平面字符（例如 Emoji）使用四字节编码。
            if ( ptr + 3 >= end ) break;
            codepoint = ((c & 0x07) << 18) | ((ptr[1] & 0x3F) << 12) |
                        ((ptr[2] & 0x3F) << 6) | (ptr[3] & 0x3F);
            ptr += 4;
        } else {
            // 无法识别的前导字节仅跳过自身，使后续合法字符仍有机会被处理。
            ptr++;
        }

        // 回调接收值语义码点，不依赖输入缓冲区之后的生命周期。
        callback(codepoint);
    }
}

}  // namespace Text
}  // namespace MMM
