#pragma once

#include <cstdint>
#include <string_view>

namespace MMM
{
namespace Text
{

// 简单的 UTF-8 解码器
// 将 UTF-8 字符串解构为 Codepoints (即 UTF-32)
// 用于字体渲染循环
inline void forEachCodepoint(std::string_view utf8Text, auto callback)
{
    const char* ptr = utf8Text.data();
    const char* end = ptr + utf8Text.size();

    while ( ptr < end )
    {
        uint32_t      codepoint = 0;
        unsigned char c         = static_cast<unsigned char>(*ptr);

        if ( c < 0x80 )
        {
            // ASCII (1 byte)
            codepoint = c;
            ptr++;
        }
        else if ( (c & 0xE0) == 0xC0 )
        {
            // 2 bytes
            if ( ptr + 1 >= end ) break;  // 安全检查
            codepoint = ((c & 0x1F) << 6) | (ptr[1] & 0x3F);
            ptr += 2;
        }
        else if ( (c & 0xF0) == 0xE0 )
        {
            // 3 bytes (中文通常在这里)
            if ( ptr + 2 >= end ) break;
            codepoint =
                ((c & 0x0F) << 12) | ((ptr[1] & 0x3F) << 6) | (ptr[2] & 0x3F);
            ptr += 3;
        }
        else if ( (c & 0xF8) == 0xF0 )
        {
            // 4 bytes (Emoji 等)
            if ( ptr + 3 >= end ) break;
            codepoint = ((c & 0x07) << 18) | ((ptr[1] & 0x3F) << 12) |
                        ((ptr[2] & 0x3F) << 6) | (ptr[3] & 0x3F);
            ptr += 4;
        }
        else
        {
            // 非法序列，跳过
            ptr++;
        }

        // 调用回调处理这个 Codepoint
        callback(codepoint);
    }
}

}  // namespace Text
}  // namespace MMM
