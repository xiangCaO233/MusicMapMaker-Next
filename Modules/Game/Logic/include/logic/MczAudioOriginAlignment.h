#pragma once

#include <string>
#include <unordered_set>

namespace MMM
{
class BeatMap;
}

namespace MMM::Logic
{

/// @brief MCZ 非 OGG 主音频原点对齐的时间计算结果。
struct MczAudioOriginAlignmentTiming {
    /// @brief 是否找到可用于对齐的有效首个 BPM 红线。
    bool success{ false };

    /// @brief 首红线按自身拍长折回一拍内后的有符号相位，单位毫秒。
    double phaseMilliseconds{ 0.0 };

    /// @brief 失败时用于日志或界面反馈的原因。
    std::string errorMessage;
};

/// @brief 计算首个 BPM 红线折回一拍内后的有符号相位。
/// @param beatMap 待读取的谱面。
/// @return 成功状态、一拍内相位和失败原因。
[[nodiscard]] MczAudioOriginAlignmentTiming
calculateMczAudioOriginAlignmentTiming(const BeatMap& beatMap);

/// @brief 在 MCZ 导出副本中把首红线和非 OGG 主音频对齐到时间原点。
///
/// 首红线先按自身拍长移动整数拍，使其进入正负一拍范围；随后全部谱面内容按
/// 剩余相位平移。与目标资源配对且最接近零点的 Main 自动采样保持在零点，供
/// 已裁切或补静音的导出音频使用；其它自动采样随谱面平移。
/// @param beatMap 仅用于导出的可修改谱面副本。
/// @param mainAudioReferences 能解析到目标 Main 音频资源的谱面引用集合；目标
/// Main 自动采样必须唯一且已经位于时间原点，否则为避免错误裁切会拒绝导出。
/// @param phaseMilliseconds calculateMczAudioOriginAlignmentTiming 返回的相位。
/// @param errorMessage 接收失败原因。
/// @return 完成对齐时返回 true。
/// @warning 用户触发的低频导出路径：会遍历完整谱面并重建引用排序，禁止在
/// 逻辑 update 或 UI 渲染热路径调用。
[[nodiscard]] bool applyMczAudioOriginAlignment(
    BeatMap&                               beatMap,
    const std::unordered_set<std::string>& mainAudioReferences,
    double phaseMilliseconds, std::string& errorMessage);

}  // namespace MMM::Logic
