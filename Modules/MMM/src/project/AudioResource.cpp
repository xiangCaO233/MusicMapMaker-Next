#include "mmm/project/AudioResource.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace MMM
{

/// @brief 将音轨用途序列化为稳定文本。
/// @param json 接收字符串值的 JSON 节点。
/// @param type 音轨用途。
void to_json(nlohmann::json& json, const AudioTrackType& type)
{
    // 仅 Effect 使用专有值，其他输入按兼容默认 Main 写出。
    json = type == AudioTrackType::Effect ? "Effect" : "Main";
}

/// @brief 从稳定文本解析音轨用途。
/// @param json 来源 JSON 节点。
/// @param type 接收结果；未知或非字符串值回退为 Main。
void from_json(const nlohmann::json& json, AudioTrackType& type)
{
    // Main 是历史资源的默认用途，只有精确 Effect 文本才改变类型。
    type = json.is_string() && json == "Effect" ? AudioTrackType::Effect
                                                : AudioTrackType::Main;
}

/// @brief 序列化音轨播放与均衡器配置。
/// @param json 接收对象的 JSON 节点。
/// @param config 待保存配置。
void to_json(nlohmann::json& json, const AudioTrackConfig& config)
{
    // 所有配置字段显式写出，使项目文件可独立重建编辑状态。
    json = nlohmann::json{ { "volume", config.volume },
                           { "playbackSpeed", config.playbackSpeed },
                           { "playbackPitch", config.playbackPitch },
                           { "muted", config.muted },
                           { "eqEnabled", config.eqEnabled },
                           { "eqPreset", config.eqPreset },
                           { "eqBandGains", config.eqBandGains },
                           { "eqBandQs", config.eqBandQs } };
}

/// @brief 兼容性反序列化音轨播放与均衡器配置。
/// @param json 来源 JSON 对象。
/// @param config 接收结果；缺失或类型错误字段保留调用前值。
void from_json(const nlohmann::json& json, AudioTrackConfig& config)
{
    // 非对象节点不应清空调用方已经设置的默认配置。
    if ( !json.is_object() ) return;

    /// @brief 读取单精度数值字段，缺失或类型不符时保留原值。
    auto readFloat = [&](const char* key, float& value) {
        // nlohmann 的 number 检查同时接受整数与浮点 JSON 数字。
        const auto iterator = json.find(key);
        if ( iterator != json.end() && iterator->is_number() ) {
            value = iterator->get<float>();
        }
    };
    /// @brief 读取布尔字段，不把数字隐式解释为开关。
    auto readBool = [&](const char* key, bool& value) {
        const auto iterator = json.find(key);
        if ( iterator != json.end() && iterator->is_boolean() ) {
            value = iterator->get<bool>();
        }
    };
    /// @brief 读取整数字段，不接受浮点截断。
    auto readInt = [&](const char* key, int& value) {
        const auto iterator = json.find(key);
        if ( iterator != json.end() && iterator->is_number_integer() ) {
            value = iterator->get<int>();
        }
    };
    /// @brief 读取单精度数值数组；数组元素无效时保留原值。
    auto readFloatVector = [&](const char* key, std::vector<float>& value) {
        const auto iterator = json.find(key);
        if ( iterator == json.end() || !iterator->is_array() ) return;

        // 先在临时数组中完成整体验证，防止非法中间元素造成部分更新。
        std::vector<float> parsedValue;
        parsedValue.reserve(iterator->size());
        for ( const auto& element : *iterator ) {
            // 任一元素非法就保留调用前完整数组。
            if ( !element.is_number() ) return;
            parsedValue.push_back(element.get<float>());
        }
        // 全部元素通过后才一次性交接新配置。
        value = std::move(parsedValue);
    };

    readFloat("volume", config.volume);
    // 标量与均衡器数组逐项读取，旧项目只提供部分字段时其余默认值仍有效。
    readFloat("playbackSpeed", config.playbackSpeed);
    readFloat("playbackPitch", config.playbackPitch);
    readBool("muted", config.muted);
    readBool("eqEnabled", config.eqEnabled);
    readInt("eqPreset", config.eqPreset);
    readFloatVector("eqBandGains", config.eqBandGains);
    // Q 值数组独立校验，不因增益数组有效就接受其中的非法元素。
    readFloatVector("eqBandQs", config.eqBandQs);
}

/// @brief 判断音频资源 JSON 是否缺少当前配置对象。
/// @param json 待检查资源节点。
/// @return 需要从旧版顶层音量字段迁移时返回 true。
bool requiresLegacyAudioResourceMigration(const nlohmann::json& json)
{
    // 非对象是损坏节点而非可迁移旧资源，交给上层格式校验拒绝。
    if ( !json.is_object() ) return false;
    // 只有缺少对象型 m_config 才需要读取旧版顶层播放字段。
    const auto configIterator = json.find("m_config");
    return configIterator == json.end() || !configIterator->is_object();
}

/// @brief 序列化项目音频资源。
/// @param json 接收对象的 JSON 节点。
/// @param resource 待保存资源。
void to_json(nlohmann::json& json, const AudioResource& resource)
{
    // 路径、用途和播放配置与稳定资源 ID 一起持久化。
    json = nlohmann::json{ { "m_id", resource.m_id },
                           { "m_path", resource.m_path },
                           { "m_type", resource.m_type },
                           { "m_config", resource.m_config } };
}

/// @brief 反序列化项目音频资源并兼容旧版顶层音量。
/// @param json 来源 JSON 节点。
/// @param resource 接收结果；无效节点得到默认空资源。
void from_json(const nlohmann::json& json, AudioResource& resource)
{
    // 先清除复用对象，缺失字段应得到类型定义中的默认值。
    resource = AudioResource{};
    if ( !json.is_object() ) return;

    // 每个标量字段先验证 JSON 类型，避免隐式转换抛出格式异常。
    if ( const auto iterator = json.find("m_id");
         iterator != json.end() && iterator->is_string() ) {
        iterator->get_to(resource.m_id);
    }
    if ( const auto iterator = json.find("m_path");
         iterator != json.end() && iterator->is_string() ) {
        // 路径保留项目文件中的 UTF-8 相对表示，不在模型层访问文件系统。
        iterator->get_to(resource.m_path);
    }
    if ( const auto iterator = json.find("m_type");
         iterator != json.end() && iterator->is_string() ) {
        // 未知类型文本由 AudioTrackType 的兼容解析回退到 Main。
        iterator->get_to(resource.m_type);
    }

    // 旧项目将音量放在资源顶层，先读取它作为新配置的迁移基线。
    if ( const auto iterator = json.find("m_volume");
         iterator != json.end() && iterator->is_number() ) {
        // 迁移值先落入新配置，随后 m_config 内显式 volume 可覆盖它。
        resource.m_config.volume = iterator->get<float>();
    }
    // 新配置存在时覆盖迁移基线，并由字段级读取保留缺失默认值。
    if ( const auto iterator = json.find("m_config");
         iterator != json.end() && iterator->is_object() ) {
        // 字段级解析保留 AudioTrackConfig 构造时建立的安全默认值。
        iterator->get_to(resource.m_config);
    }
}

}  // namespace MMM
