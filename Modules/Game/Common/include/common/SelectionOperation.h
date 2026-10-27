#pragma once

#include <cstdint>

namespace MMM::Logic
{
/// @brief 纯鼠标选择手势对既有选择集合的处理方式。
/// @details 与框选几何的相交/包含规则独立，不表示键盘修饰键。
enum class SelectionOperation : std::uint8_t {
    /// @brief 以本次选择替换原有集合。
    Replace,
    /// @brief 将命中对象加入集合，已经选中的对象继续保留。
    Add,
    /// @brief 将命中对象移出集合，不改变其他对象。
    Subtract
};
}  // namespace MMM::Logic
