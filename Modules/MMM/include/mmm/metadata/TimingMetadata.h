#pragma once

#include "mmm/metadata/StringHash.h"
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace MMM
{
/// @brief 标识时间轴扩展属性的来源格式，保持历史枚举顺序。
enum class TimingMetadataType {
    /// @brief osu! 来源字段。
    OSU,
    /// @brief Rhythm Master 来源字段。
    RM,
    /// @brief Malody 来源字段。
    MALODY,
};

/// @brief 按值保留时间轴对象的来源属性，维持原有复制及析构语义。
class TimingMetadata
{
public:
    /// @brief 构造空时间轴元数据。
    TimingMetadata() = default;
    /// @brief 保持既有多态析构契约。
    virtual ~TimingMetadata() = default;

    /// @brief 按来源和字段名称保存原始文本。
    std::unordered_map<TimingMetadataType,
                       std::unordered_map<std::string, std::string, StringHash,
                                          std::equal_to<>>>
        timing_properties;

    /**
     * @brief 获取数据
     * @param source 来源类型 (如 TimingMetadataType::OSU)
     * @param key 属性键名
     * @param default_value 找不到时的默认返回值
     */
    template<typename T>
    T get_value(TimingMetadataType source, const std::string& key,
                T default_value = T())
    {
        // 1. 先查找来源 (OSU 或 MALODY)
        auto properties_it = timing_properties.find(source);
        if ( properties_it == timing_properties.end() ) return default_value;

        // 2. 在该来源的 Map 中查找具体的 key
        auto key_it = properties_it->second.find(key);
        if ( key_it == properties_it->second.end() ) return default_value;

        // 3. 处理数据转换
        if constexpr ( std::is_same_v<T, std::string> ) {
            // 如果目标类型就是 string，直接返回
            return key_it->second;
        } else {
            // 使用 istringstream 进行类型转换 (int, float, double 等)
            std::istringstream iss(key_it->second);
            T                  value;
            // 如果转换失败（例如字符串内容无法解析为数字），返回默认值
            if ( !(iss >> value) ) return default_value;
            return value;
        }
    }
};
}  // namespace MMM
