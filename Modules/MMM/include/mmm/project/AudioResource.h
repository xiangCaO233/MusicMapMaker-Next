#pragma once

#include "mmm/project/AudioTrackConfig.h"
#include "mmm/project/AudioTrackType.h"

#include <nlohmann/json_fwd.hpp>
#include <string>

namespace MMM
{

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
