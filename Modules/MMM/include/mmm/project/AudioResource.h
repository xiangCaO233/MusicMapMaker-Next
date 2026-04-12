#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace MMM
{

/// @brief 音轨类型：决定了该轨道是否能作为谱面创作的基础
enum class AudioTrackType {
    Main,   ///< 主音轨 (核心曲目，一个项目可以有多个主轨道供不同版本谱面使用)
    Effect  ///< 音效/辅助轨道 (如打击音、环境音、伴奏等)
};

NLOHMANN_JSON_SERIALIZE_ENUM(AudioTrackType,
                             {
                                 { AudioTrackType::Main, "Main" },
                                 { AudioTrackType::Effect, "Effect" },
                             })

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

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(AudioTrackConfig, volume, playbackSpeed,
                                   playbackPitch, muted, eqEnabled, eqPreset,
                                   eqBandGains, eqBandQs)
};

/// @brief 项目管理的音频资源
struct AudioResource {
    /// @brief 资源唯一标识 (ID)，用于被谱面引用
    std::string m_id;

    /// @brief 音频文件路径 (相对于项目根目录的相对路径)
    std::string m_path;

    /// @brief 轨道类型
    AudioTrackType m_type{ AudioTrackType::Main };

    /// @brief 音轨配置 (持久化项)
    AudioTrackConfig m_config;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(AudioResource, m_id, m_path, m_type,
                                   m_config)
};

}  // namespace MMM
