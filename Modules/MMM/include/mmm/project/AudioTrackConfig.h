#pragma once

#include <nlohmann/json_fwd.hpp>
#include <vector>

namespace MMM
{

/// @brief 单个音频轨道的详细配置
struct AudioTrackConfig {
    /// @brief 轨道音量 (0.0 ~ 1.0)
    float volume{ 0.5f };

    /// @brief 播放速度倍率 (0.5 ~ 2.0)
    float playbackSpeed{ 1.0f };

    /// @brief 播放音高偏移 (-24.0 ~ 24.0)
    float playbackPitch{ 0.0f };

    /// @brief 是否静音
    bool muted{ false };

    /// @brief EQ 是否启用
    bool eqEnabled{ false };

    /// @brief EQ 预设类型 (0=None, 1=10-Band, 2=15-Band)
    int eqPreset{ 0 };

    /// @brief 每个频段的增益 (dB)
    std::vector<float> eqBandGains{};

    /// @brief 每个频段的 Q 值
    std::vector<float> eqBandQs{};
};

/// @brief 将音轨配置序列化为当前项目格式。
/// @param json 输出 JSON 对象。
/// @param config 待序列化的音轨配置。
void to_json(nlohmann::json& json, const AudioTrackConfig& config);

/// @brief 从项目 JSON 中读取音轨配置，并为旧配置缺失字段保留默认值。
/// @param json 输入 JSON 对象。
/// @param config 接收反序列化结果的音轨配置。
void from_json(const nlohmann::json& json, AudioTrackConfig& config);

}  // namespace MMM
