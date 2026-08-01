#include "mmm/project/AudioResource.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace MMM
{

void to_json(nlohmann::json& json, const AudioTrackType& type)
{
    json = type == AudioTrackType::Effect ? "Effect" : "Main";
}

void from_json(const nlohmann::json& json, AudioTrackType& type)
{
    type = json.is_string() && json == "Effect" ? AudioTrackType::Effect
                                                : AudioTrackType::Main;
}

void to_json(nlohmann::json& json, const AudioTrackConfig& config)
{
    json = nlohmann::json{ { "volume", config.volume },
                           { "playbackSpeed", config.playbackSpeed },
                           { "playbackPitch", config.playbackPitch },
                           { "muted", config.muted },
                           { "eqEnabled", config.eqEnabled },
                           { "eqPreset", config.eqPreset },
                           { "eqBandGains", config.eqBandGains },
                           { "eqBandQs", config.eqBandQs } };
}

void from_json(const nlohmann::json& json, AudioTrackConfig& config)
{
    if ( !json.is_object() ) return;

    /// @brief 读取单精度数值字段。
    auto readFloat = [&](const char* key, float& value) {
        const auto iterator = json.find(key);
        if ( iterator != json.end() && iterator->is_number() ) {
            value = iterator->get<float>();
        }
    };
    /// @brief 读取布尔字段。
    auto readBool = [&](const char* key, bool& value) {
        const auto iterator = json.find(key);
        if ( iterator != json.end() && iterator->is_boolean() ) {
            value = iterator->get<bool>();
        }
    };
    /// @brief 读取整数字段。
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

        std::vector<float> parsedValue;
        parsedValue.reserve(iterator->size());
        for ( const auto& element : *iterator ) {
            if ( !element.is_number() ) return;
            parsedValue.push_back(element.get<float>());
        }
        value = std::move(parsedValue);
    };

    readFloat("volume", config.volume);
    readFloat("playbackSpeed", config.playbackSpeed);
    readFloat("playbackPitch", config.playbackPitch);
    readBool("muted", config.muted);
    readBool("eqEnabled", config.eqEnabled);
    readInt("eqPreset", config.eqPreset);
    readFloatVector("eqBandGains", config.eqBandGains);
    readFloatVector("eqBandQs", config.eqBandQs);
}

bool requiresLegacyAudioResourceMigration(const nlohmann::json& json)
{
    if ( !json.is_object() ) return false;
    const auto configIterator = json.find("m_config");
    return configIterator == json.end() || !configIterator->is_object();
}

void to_json(nlohmann::json& json, const AudioResource& resource)
{
    json = nlohmann::json{ { "m_id", resource.m_id },
                           { "m_path", resource.m_path },
                           { "m_type", resource.m_type },
                           { "m_config", resource.m_config } };
}

void from_json(const nlohmann::json& json, AudioResource& resource)
{
    resource = AudioResource{};
    if ( !json.is_object() ) return;

    if ( const auto iterator = json.find("m_id");
         iterator != json.end() && iterator->is_string() ) {
        iterator->get_to(resource.m_id);
    }
    if ( const auto iterator = json.find("m_path");
         iterator != json.end() && iterator->is_string() ) {
        iterator->get_to(resource.m_path);
    }
    if ( const auto iterator = json.find("m_type");
         iterator != json.end() && iterator->is_string() ) {
        iterator->get_to(resource.m_type);
    }

    if ( const auto iterator = json.find("m_volume");
         iterator != json.end() && iterator->is_number() ) {
        resource.m_config.volume = iterator->get<float>();
    }
    if ( const auto iterator = json.find("m_config");
         iterator != json.end() && iterator->is_object() ) {
        iterator->get_to(resource.m_config);
    }
}

}  // namespace MMM
