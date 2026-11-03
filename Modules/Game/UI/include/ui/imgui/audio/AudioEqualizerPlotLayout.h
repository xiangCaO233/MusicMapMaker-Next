#pragma once
#include <algorithm>

namespace MMM::UI
{
/// @brief 触屏频段的两个独立操作高度，不随当前剩余屏幕高度缩短。
struct AudioEqualizerBandHeights {
    /// @brief 增益需要较长行程，方便在正负 24 dB 范围内细调。
    float m_gain;
    /// @brief Q 同样保持独立触摸行程，不能被增益条挤成短块。
    float m_q;
};
/// @brief 由 DPI 与字号确定触屏滑条尺寸，布局滚动不能改变拖动灵敏度。
/// @warning 每帧常量数学，不分配、不读取音频或配置状态。
inline AudioEqualizerBandHeights audioEqualizerBandHeights(float scale,
                                                           float fontHeight)
{
    // 两种约束取较大值，兼容高密度屏幕与用户额外放大的字体。
    return { std::max(220.0f * scale, 7 * fontHeight),
             std::max(144.0f * scale, 4 * fontHeight) };
}

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
