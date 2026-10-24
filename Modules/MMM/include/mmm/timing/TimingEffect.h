#pragma once

namespace MMM
{
/// @brief 模型与渲染 DTO 共用的时间线效果种类。
/// @details 保持原枚举顺序及默认 int 底层类型，兼容现有存储与分发。
/// 效果参数和插值数据由各自值类型持有，不在此处引入完整 Timing。
enum class TimingEffect {
    /// @brief bpm 效果。
    BPM,
    /// @brief scroll 效果。
    SCROLL,
    /// @brief Malody jump 瞬移效果。
    JUMP,
    /// @brief Malody hs 音符速度效果。
    HS,
    /// @brief 冻结谱面与全部播放采样；参数为持续秒数。
    STOP,
};
}  // namespace MMM
