#pragma once

#include <nlohmann/json_fwd.hpp>

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

}  // namespace MMM
