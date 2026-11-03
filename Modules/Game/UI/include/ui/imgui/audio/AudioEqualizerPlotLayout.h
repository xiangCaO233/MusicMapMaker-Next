#pragma once
#include <algorithm>

namespace MMM::UI
{
/// @brief 计算 EQ 响应图外框高度；桌面保持原有像素高度。
/// @param requestedHeight 触屏用户选择的逻辑高度，不写入音频或谱面参数。
/// @param scale 当前窗口内容倍率，逻辑高度只在这里换算一次。
/// @param fontHeight 当前实际字号，防止坐标轴占满高 DPI 绘图区。
/// @param touch 是否使用独立移动布局。
/// @details 至少十行文字高度同时容纳刻度、轴标题和可读的数据区域。
/// 更大的用户选择仍然生效；面板不足时由外层内容滚动而非压缩曲线。
/// @warning UI 每帧常量级测量，无分配、配置写入或音频线程访问。
inline float audioEqualizerPlotHeight(float requestedHeight, float scale,
                                      float fontHeight, bool touch)
{
    // 桌面沿用原高度，不借触屏适配改变原有窗口节奏。
    if ( !touch ) return 150.0f;
    return std::max(
        { 240.0f * scale, requestedHeight * scale, fontHeight * 10.0f });
}
}  // namespace MMM::UI
