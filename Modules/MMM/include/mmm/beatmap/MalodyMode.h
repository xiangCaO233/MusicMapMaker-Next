#pragma once

#include <cstdint>

namespace MMM
{

/// @brief 当前导出器支持的 Malody 谱面模式。
/// @details 枚举值直接对应 Malody `meta.mode`，不得按声明顺序重新编号。
enum class MalodyMode : std::uint8_t {
    /// @brief 多轨下落式 Key 模式。
    Key = 0,
    /// @brief 自由轨迹 Slide 模式。
    Slide = 7,
};

/// @brief 将 Malody 模式转换为格式元数据中的整数值。
/// @param mode Malody 谱面模式。
/// @return Malody meta.mode 对应的整数值。
constexpr int malodyModeValue(MalodyMode mode)
{
    // 显式转换保留强类型边界，序列化层才暴露格式整数。
    return static_cast<int>(mode);
}

}  // namespace MMM
