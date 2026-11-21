#pragma once

#include <optional>
#include <string>

namespace MMM::Bms
{
/// @brief 把未经修改的 BMS 字节解码成 UTF-8。
/// @param text 待解码文本，调用方确保不超过平台转换器可用大小。
/// @param myO2 旧编码回退是否按 MyO2 的 GBK 约定处理。
/// @return 编码非法时为空，调用方保留原始文件。
/// @warning 低频文件操作，使用系统编码转换器并分配输出缓冲。
[[nodiscard]] std::optional<std::string> decodeBmsText(std::string text,
                                                       bool        myO2);
}  // namespace MMM::Bms
