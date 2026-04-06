#pragma once
#include <nlohmann/json.hpp>
#include <string>

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

/// @brief 项目管理的音频资源
struct AudioResource {
    /// @brief 资源唯一标识 (ID)，用于被谱面引用
    std::string m_id;

    /// @brief 音频文件路径 (相对于项目根目录的相对路径)
    std::string m_path;

    /// @brief 轨道类型
    AudioTrackType m_type{ AudioTrackType::Main };

    /// @brief 默认预设音量 (0.0 ~ 1.0)
    float m_volume{ 1.0f };

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(AudioResource, m_id, m_path, m_type,
                                   m_volume)
};

}  // namespace MMM
