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

/// @brief 每个平台允许的最小逻辑高度，供尺寸控件和绘图布局共同使用。
inline constexpr int DESKTOP_EQ_PLOT_MIN_HEIGHT = 120;
/// @brief 触屏使用更大的最小绘图区，轴文字不会挤占全部内容。
inline constexpr int TOUCH_EQ_PLOT_MIN_HEIGHT = 240;
/// @brief 限制可调图表高度，超过窗口的内容仍由外层滚动。
inline constexpr int EQ_PLOT_MAX_HEIGHT = 640;

/// @brief 计算 EQ 响应图外框高度，逻辑尺寸只换算一次。
/// @param requestedHeight 用户选择的逻辑高度，不写入音频或谱面参数。
/// @param scale 当前窗口内容倍率，逻辑高度只在这里换算一次。
/// @param fontHeight 当前实际字号，防止坐标轴占满高 DPI 绘图区。
/// @param touch 是否使用独立移动布局。
/// @details 为刻度、轴标题和实际绘图区预留字号相关的最小高度。
/// 更大的用户选择仍然生效；面板不足时由外层内容滚动而非压缩曲线。
/// @warning UI 每帧常量级测量，无分配、配置写入或音频线程访问。
inline float audioEqualizerPlotHeight(float requestedHeight, float scale,
                                      float fontHeight, bool touch)
{
    const auto minHeight = static_cast<float>(
        touch ? TOUCH_EQ_PLOT_MIN_HEIGHT : DESKTOP_EQ_PLOT_MIN_HEIGHT);
    // 对异常或过小请求钳制后，再按实际字号避免坐标装饰挤掉数据区。
    const float logicalHeight = std::clamp(
        requestedHeight, minHeight, static_cast<float>(EQ_PLOT_MAX_HEIGHT));
    return std::max(logicalHeight * scale, fontHeight * (touch ? 10.0f : 9.0f));
}
}  // namespace MMM::UI
