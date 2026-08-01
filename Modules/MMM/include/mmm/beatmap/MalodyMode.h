#pragma once

#include <cstdint>

namespace MMM
{

/// @brief 当前导出器支持的 Malody 谱面模式。
enum class MalodyMode : std::uint8_t {
    Key   = 0,
    Slide = 7,
};

/// @brief 将 Malody 模式转换为格式元数据中的整数值。
/// @param mode Malody 谱面模式。
/// @return Malody meta.mode 对应的整数值。
constexpr int malodyModeValue(MalodyMode mode)
{
    return static_cast<int>(mode);
}

}  // namespace MMM
