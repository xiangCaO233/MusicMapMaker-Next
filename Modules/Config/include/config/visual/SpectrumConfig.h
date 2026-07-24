#pragma once

#include <cstdint>
#include <nlohmann/json_fwd.hpp>

namespace MMM::Config
{

/// @brief 频谱图生成精细度。
enum class SpectrumDetailLevel {
    Performance,
    Balanced,
    Fine,
    Ultra,
    Extreme,
    Experimental,
};

/// @brief 频谱图精细度对应的生成参数。
struct SpectrumDetailProfile {
    /// @brief 时间分辨率，单位为段/秒。
    double segmentsPerSecond{ 100.0 };

    /// @brief 频率方向分箱数。
    int frequencyBins{ 128 };
};

/// @brief 获取频谱图精细度对应的生成参数。
/// @param level 频谱图精细度。
/// @return 频谱图生成参数。
inline SpectrumDetailProfile spectrumDetailProfile(SpectrumDetailLevel level)
{
    switch ( level ) {
    case SpectrumDetailLevel::Performance: return { 40.0, 64 };
    case SpectrumDetailLevel::Balanced: return { 64.0, 96 };
    case SpectrumDetailLevel::Fine: return { 96.0, 128 };
    case SpectrumDetailLevel::Ultra: return { 160.0, 192 };
    case SpectrumDetailLevel::Extreme: return { 240.0, 256 };
    case SpectrumDetailLevel::Experimental: return { 360.0, 384 };
    }
    return { 64.0, 96 };
}

/// @brief 估算指定频谱图精细度每分钟需要的纹理显存。
/// @param level 频谱图精细度。
/// @param channelCount 频谱图纹理通道数量。
/// @param bytesPerTexel 每个纹理像素占用的字节数。
/// @return 每分钟纹理显存字节数，不含驱动对齐和描述符开销。
inline std::uint64_t estimateSpectrumTextureBytesPerMinute(
    SpectrumDetailLevel level, std::uint32_t channelCount,
    std::uint32_t bytesPerTexel = 1)
{
    const auto profile = spectrumDetailProfile(level);
    return static_cast<std::uint64_t>(
        profile.segmentsPerSecond * 60.0 *
        static_cast<double>(profile.frequencyBins) *
        static_cast<double>(bytesPerTexel) * static_cast<double>(channelCount));
}

/// @brief 将频谱精细度序列化为 JSON。
void to_json(nlohmann::json& json, const SpectrumDetailLevel& level);
/// @brief 从 JSON 读取频谱精细度。
void from_json(const nlohmann::json& json, SpectrumDetailLevel& level);

}  // namespace MMM::Config
