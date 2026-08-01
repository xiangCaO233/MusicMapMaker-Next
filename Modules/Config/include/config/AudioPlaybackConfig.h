#pragma once

#include <nlohmann/json_fwd.hpp>

namespace MMM::Config
{

/// @brief 音频播放后端偏好。
enum class AudioPlaybackBackend {
    SDL,    ///< SDL 音频后端。
    OpenAL  ///< OpenAL Soft 音频后端。
};

/// @brief OpenAL 空间化输出配置。
struct OpenALSpatialConfig {
    /// @brief 是否启用 OpenAL 空间化输出。
    bool enabled{ false };

    /// @brief 声源方向 X 分量。
    float directionX{ 0.0f };

    /// @brief 声源方向 Y 分量。
    float directionY{ 0.0f };

    /// @brief 声源方向 Z 分量。
    float directionZ{ -1.0f };

    /// @brief 声源距离。
    float distance{ 1.0f };

    /// @brief OpenAL 参考距离。
    float referenceDistance{ 1.0f };

    /// @brief OpenAL 最大距离。
    float maxDistance{ 100.0f };

    /// @brief OpenAL 距离衰减倍率。
    float rolloffFactor{ 1.0f };
};

/// @brief 将音频播放后端序列化为稳定文本。
void to_json(nlohmann::json& json, const AudioPlaybackBackend& backend);
/// @brief 从稳定文本读取音频播放后端。
void from_json(const nlohmann::json& json, AudioPlaybackBackend& backend);

/// @brief 将 OpenAL 空间化配置序列化为 JSON。
void to_json(nlohmann::json& json, const OpenALSpatialConfig& config);
/// @brief 从 JSON 读取 OpenAL 空间化配置。
void from_json(const nlohmann::json& json, OpenALSpatialConfig& config);

}  // namespace MMM::Config
