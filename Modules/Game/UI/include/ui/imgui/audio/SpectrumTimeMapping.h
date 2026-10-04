#pragma once

namespace MMM::UI
{
/// @brief 两种对数图共用的 FFT 分析窗长度，单位为采样帧。
/// @note 缓存列对应窗口读取起点，显示时必须补偿半窗才能对齐音频内容。
inline constexpr unsigned SPECTRUM_FFT_WINDOW_FRAMES = 2048U;

/// @brief 将视觉时间映射到频谱缓存列所代表的音频窗口起点。
/// @param visualTime 当前图表横轴时间，单位为秒。
/// @param spectrumOffset 用户配置的频谱专用视觉偏移，单位为秒。
/// @param sampleRate FFT 输入的实际采样率，不使用音频文件原始采样率。
/// @return 可乘缓存段密度的音频时间；允许负数用于裁剪音频开始前的区域。
/// @details 窗口能量表示读取区间的中心，纹理列却按读取起点存储。
/// 因此需先移除频谱专用偏移，再扣除半窗；全局播放视觉偏移只用于播放头。
/// @warning 每帧图表映射可调用；仅执行标量运算，不分配、不锁定资源。
constexpr double spectrumWindowStartAtVisualTime(double visualTime,
                                                 double spectrumOffset,
                                                 double sampleRate)
{
    // 未准备好的采样格式不能产生除零；此时仅应用用户的频谱偏移。
    const double centerOffset =
        sampleRate > 0.0 ? SPECTRUM_FFT_WINDOW_FRAMES * 0.5 / sampleRate : 0.0;
    // 保持与音频工具既有符号一致：正偏移将频谱内容向较晚时间移动。
    return visualTime - spectrumOffset - centerOffset;
}
}  // namespace MMM::UI
