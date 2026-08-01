#pragma once
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace MMM
{

/// @brief 音轨类型：决定了该轨道是否能作为谱面创作的基础
enum class AudioTrackType {
    Main,   ///< 主音轨 (核心曲目，一个项目可以有多个主轨道供不同版本谱面使用)
    Effect  ///< 音效/辅助轨道 (如打击音、环境音、伴奏等)
};

/// @brief 将音轨类型序列化为稳定文本。
/// @param json 输出 JSON 值。
/// @param type 待序列化的音轨类型。
void to_json(nlohmann::json& json, const AudioTrackType& type);

/// @brief 从稳定文本读取音轨类型，未知值回退到主音轨。
/// @param json 输入 JSON 值。
/// @param type 接收反序列化结果的音轨类型。
void from_json(const nlohmann::json& json, AudioTrackType& type);

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

/// @brief 判断音频资源 JSON 是否缺少当前格式的 m_config 对象。
/// @param json 待检查的音频资源 JSON。
/// @return 需要按旧版或未知格式迁移时返回 true。
[[nodiscard]] bool requiresLegacyAudioResourceMigration(
    const nlohmann::json& json);

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
};

/// @brief 将音频资源序列化为当前 m_config 项目格式。
/// @param json 输出 JSON 对象。
/// @param resource 待序列化的音频资源。
void to_json(nlohmann::json& json, const AudioResource& resource);

/// @brief 读取音频资源，并兼容旧版顶层 m_volume 字段。
/// @param json 输入 JSON 对象。
/// @param resource 接收反序列化结果的音频资源。
void from_json(const nlohmann::json& json, AudioResource& resource);

}  // namespace MMM
