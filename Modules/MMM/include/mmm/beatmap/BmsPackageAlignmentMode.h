#pragma once

namespace MMM
{
/// @brief BMS 谱包首拍对齐的两种互斥策略。
/// @note 两种策略只校正共同原点，不能重新吸附单个音符。
/// @note 首 BPM 固定在零点，后续所有红线必须随物件使用相同位移。
/// @note 该选项只作用于导出副本，不改变项目中的素材与时间轴。
enum class BmsPackageAlignmentMode {
    /// @brief 平移物件，并通过裁剪或补静音让主音乐从零点播放。
    RewriteAudio,
    /// @brief 音频保持原字节，BGM 触发与物件共同后移到零点拍网格。
    ShiftEvents,
};
}  // namespace MMM
