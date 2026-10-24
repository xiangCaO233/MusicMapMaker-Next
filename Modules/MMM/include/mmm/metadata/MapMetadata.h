#pragma once

#include "mmm/metadata/StringHash.h"
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace MMM
{
/// @brief 标识扩展属性的来源格式，保持历史枚举顺序。
enum class MapMetadataType {
    /// @brief osu! 来源字段。
    OSU,
    /// @brief Malody 来源字段。
    MALODY,
    /// @brief Rhythm Master 来源字段。
    RM,
    /// @brief BMS/PMS 来源键位语义，原生保存后继续用于标识转盘。
    BMS,
};

/// @brief 按值保存谱面级来源格式扩展属性。
/// 属性表属于当前对象，复制后不与源对象共享可变状态。
/// @details 类型拆分只改变包含边界，保留虚析构及默认复制语义。
class MapMetadata
{
public:
    /// @brief 构造空谱面元数据。
    MapMetadata() = default;
    /// @brief 释放谱面元数据。
    virtual ~MapMetadata() = default;

    /// @brief 按来源和字段名称保存原始文本，供格式导入导出往返使用。
    std::unordered_map<MapMetadataType,
                       std::unordered_map<std::string, std::string, StringHash,
                                          std::equal_to<>>>
        map_properties;

    /// @brief 读取并转换指定来源的谱面属性。
    /// @param source 扩展属性所属来源格式。
    /// @param key 来源域内的字段名称。
    /// @param default_value 来源或字段不存在时返回的默认值。
    /// @return 属性缺失时返回调用方给出的默认值。
    /// @note 数值转换继续使用原有流提取语义，不在依赖拆分中改变失败处理。
    template<typename T>
    T get_value(MapMetadataType source, const std::string& key,
                T default_value = T())
    {
        // 两级查询均不插入默认键，读取操作不会污染保留属性。
        auto properties_it = map_properties.find(source);
        if ( properties_it == map_properties.end() ) return default_value;
        auto key_it = properties_it->second.find(key);
        if ( key_it == properties_it->second.end() ) return default_value;
        if constexpr ( std::is_same_v<T, std::string> ) {
            // 类型为字符串时整个返回
            return key_it->second;
        } else {
            // 历史接口对转换失败返回值初始化结果，调用方应优先提供默认值。
            // 直接从保存文本执行既有模板转换，不增加缓存或间接存储。
            std::istringstream iss(key_it->second);
            T                  value;
            iss >> value;
            return value;
        }
    }
};

}  // namespace MMM
