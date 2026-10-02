#pragma once

namespace MMM
{
class TimingFunction;
}
namespace MMM::Canvas
{
/// @brief 从已编译表达式绘制可缩放数学公式，含分式、根式、上标和绝对值。
/// @details 积分、求和及累乘的上下限独立排版，整体保持阅读字号并允许横向滚动。
/// @note 预览借用同一数学树，禁止单独解析字符串或用替换文本猜测结构。
/// @warning 每帧编辑窗口路径；最多 256 个节点，用栈上度量缓存避免堆分配。
void renderTimingFormula(const TimingFunction& function);
}  // namespace MMM::Canvas
