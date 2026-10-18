#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
// 禁止 Windows 头定义 min/max 宏，保持后续标准库算法可用。
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <iconv.h>
#endif

namespace MMM::Bms
{
/// @brief 检查文本是否为严格 UTF-8，避免依赖系统默认区域设置。
/// @param text 已受文件大小上限约束的字节串。
/// @return 不含非法续字节、过长编码、代理码点时返回 true。
/// @note ASCII 同时兼容旧 BMS 编码，可直接保留。
inline bool validUtf8(std::string_view text)
{
    // 不依赖 C 字符串结束符，整段输入都要验证。
    // 每次先验证剩余字节数，再读取续字节，截断序列不能越界。
    // 验证阶段不修改原数据，失败后仍可把完整原文交给 CP932 转换器。
    // 四字节编码仅允许 F0–F4，其他首字节不能映射到 Unicode 标量值。
    for ( std::size_t i = 0; i < text.size(); ) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if ( first < 0x80 ) continue;
        // 首字节限定 Unicode 最大值；C0/C1 会构成过长 ASCII 编码。
        const int count = first >= 0xC2 && first <= 0xDF   ? 1
                          : first >= 0xE0 && first <= 0xEF ? 2
                          : first >= 0xF0 && first <= 0xF4 ? 3
                                                           : -1;
        if ( count < 0 || text.size() - i < static_cast<std::size_t>(count) )
            return false;
        std::uint32_t value = first & ((1U << (6 - count)) - 1U);
        for ( int j = 0; j < count; ++j ) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ( (next & 0xC0) != 0x80 ) return false;
            value = (value << 6) | (next & 0x3F);
        }
        // 拒绝 UTF-16 代理区和超出 Unicode 范围的数值。
        // 同时检查最短编码，不把非法 UTF-8 当成有效资源名称。
        if ( value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF) ||
             (count == 2 && value < 0x800) || (count == 3 && value < 0x10000) )
            return false;
    }
    return true;
}

/// @brief 将传统 CP932 BMS 文本转换为项目统一的 UTF-8。
/// @param text 原始文件文本，调用者已限制到 64 MiB。
/// @return UTF-8 原文或转换结果；非法编码返回空 optional。
/// @note 先验证整份文件，不能逐行猜测编码，否则双字节路径可能被拆错。
/// @note CP932 为传统日文 BMS 的常用 Windows 编码，不使用本机 ACP。
/// @warning 仅在低频文件加载执行，涉及分配及操作系统转换资源。
inline std::optional<std::string> decodeText(std::string text)
{
    // UTF-8 优先，导出器生成的 Unicode 文件不应再次按旧代码页解码。
    // 没有编码声明的 ASCII 文件在两种编码下完全等价。
    // 只有整份 UTF-8 校验失败才启用传统代码页回退。
    // 转换失败保留失败身份，不能替换成问号后继续寻找错误的 WAV 文件。
    if ( validUtf8(text) ) return text;
#ifdef _WIN32
    // 查询长度后再分配，UTF-16 字符数与 UTF-8 字节数不能混用。
    // 显式输入长度不包含结尾 NUL，保留换行供后续逐行解析。
    // 调用者限制文件大小低于 int 上限，Win32 长度转换不会截断。
    // MB_ERR_INVALID_CHARS 使缺失尾字节的日文字符明确失败。
    // 先查询 UTF-16 容量，再查询 UTF-8 容量，两个单位不可相互代替。
    // 原文和输出使用独立存储，不向输入缓冲区原地扩写。
    const int inputSize = static_cast<int>(text.size());
    const int wideSize  = MultiByteToWideChar(
        932, MB_ERR_INVALID_CHARS, text.data(), inputSize, nullptr, 0);
    if ( wideSize <= 0 ) return std::nullopt;
    std::wstring wide(wideSize, L'\0');
    if ( MultiByteToWideChar(932,
                             MB_ERR_INVALID_CHARS,
                             text.data(),
                             inputSize,
                             wide.data(),
                             wideSize) != wideSize )
        return std::nullopt;
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), wideSize, nullptr, 0, nullptr, nullptr);
    if ( size <= 0 ) return std::nullopt;
    std::string result(size, '\0');
    if ( WideCharToMultiByte(CP_UTF8,
                             0,
                             wide.data(),
                             wideSize,
                             result.data(),
                             size,
                             nullptr,
                             nullptr) != size )
        return std::nullopt;
    return result;
#else
    // POSIX 平台使用系统转换器，不引入可变的全局 locale。
    // CP932 的单字节半角字符最多膨胀成三个 UTF-8 字节，四倍容量留足空间。
    // 转换器仅属于本次载入，不在不同线程或不同文件之间共享状态。
    // 打开失败时返回错误，不偷偷退回系统当前语言代码页。
    // 输出没有 transliteration 选项，禁止把无法转换的字符近似成其他字符。
    // iconv 更新指针与剩余长度，最终同时检查错误码和输入是否完整消费。
    // 转换输出不包含额外终止符，std::string 的逻辑长度单独收缩。
    auto converter = iconv_open("UTF-8", "CP932");
    if ( converter == reinterpret_cast<iconv_t>(-1) ) return std::nullopt;
    std::string result(text.size() * 4, '\0');
    char*       input     = text.data();
    char*       output    = result.data();
    std::size_t inputLeft = text.size(), outputLeft = result.size();
    const auto  converted =
        iconv(converter, &input, &inputLeft, &output, &outputLeft);
    // 无论成功还是失败都关闭系统句柄，不在错误分支遗留转换状态。
    // CP932 没有跨文件移位状态，不需要用空输入刷新末尾序列。
    iconv_close(converter);
    if ( converted == static_cast<std::size_t>(-1) || inputLeft != 0 )
        return std::nullopt;
    result.resize(result.size() - outputLeft);
    return result;
#endif
}
}  // namespace MMM::Bms
