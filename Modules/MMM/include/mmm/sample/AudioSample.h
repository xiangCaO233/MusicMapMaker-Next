#pragma once

#include "mmm/Metadata.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace MMM
{

/// @brief 采样对象附加元数据的来源格式。
enum class SampleMetadataType {
    MALODY,
    MMM,
};

/// @brief 保存采样对象无法直接映射到通用字段的来源格式属性。
class SampleMetadata
{
public:
    /// @brief 构造空采样元数据。
    SampleMetadata() = default;

    /// @brief 析构采样元数据。
    virtual ~SampleMetadata() = default;

    /// @brief 按来源格式保存采样对象的附加字符串属性。
    std::unordered_map<SampleMetadataType,
                       std::unordered_map<std::string, std::string, StringHash,
                                          std::equal_to<>>>
        sample_properties;

    /// @brief 获取指定来源的采样属性。
    /// @param source 属性来源格式。
    /// @param key 属性名称。
    /// @param defaultValue 属性不存在或转换失败时的默认值。
    /// @return 转换后的属性值。
    template<typename T>
    T getValue(SampleMetadataType source, const std::string& key,
               T defaultValue = T()) const
    {
        const auto propertiesIt = sample_properties.find(source);
        if ( propertiesIt == sample_properties.end() ) return defaultValue;
        const auto keyIt = propertiesIt->second.find(key);
        if ( keyIt == propertiesIt->second.end() ) return defaultValue;
        if constexpr ( std::is_same_v<T, std::string> ) {
            return keyIt->second;
        } else {
            std::istringstream stream(keyIt->second);
            T                  value;
            if ( !(stream >> value) ) return defaultValue;
            return value;
        }
    }
};

/// @brief 玩家物件命中时触发的单个采样绑定。
struct AudioSampleBinding {
    /// @brief 项目音频资源标识。
    std::string m_audioResourceId;

    /// @brief 播放音量倍率，1.0 表示原始音量。
    float m_volume{ 1.0F };
};

/// @brief 时间线上无需玩家操作即可自动播放的采样对象。
struct AudioSampleEvent {
    /// @brief 采样对象所在拍号位置换算出的锚点时间，单位为毫秒。
    double m_timestamp{ 0.0 };

    /// @brief 相对锚点的有符号播放偏移，负值提前、正值延后，单位为整数毫秒。
    std::int64_t m_offsetMs{ 0 };

    /// @brief 统一轨道空间中的绝对轨道索引。
    uint32_t m_track{ 0 };

    /// @brief 项目音频资源标识。
    std::string m_audioResourceId;

    /// @brief 播放音量倍率，1.0 表示原始音量。
    float m_volume{ 1.0F };

    /// @brief 采样对象附加元数据。
    SampleMetadata m_metadata;

    /// @brief 获取应用有符号偏移后的实际播放时间。
    /// @return 实际播放时间，单位为毫秒。
    [[nodiscard]] double effectiveTimestamp() const
    {
        return m_timestamp + static_cast<double>(m_offsetMs);
    }
};

}  // namespace MMM
