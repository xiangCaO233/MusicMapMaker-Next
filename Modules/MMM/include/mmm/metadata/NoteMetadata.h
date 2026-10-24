#pragma once

#include "mmm/metadata/StringHash.h"
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace MMM
{
/// @brief 标识扩展属性的来源格式，保持历史枚举顺序。
enum class NoteMetadataType {
    /// @brief osu! 来源字段。
    OSU,
    /// @brief Malody 来源字段。
    MALODY,
    /// @brief Rhythm Master 来源字段。
    RM,
    /// @brief MusicMapMaker 原生来源字段。
    MMM,
};

/// @brief 按值保存来源格式的扩展属性，不与其他对象共享可变属性表。
/// @details 类型拆分只改变包含边界，保留虚析构及默认复制语义。
class NoteMetadata
{
public:
    /// @brief 构造空物件元数据。
    NoteMetadata() = default;
    /// @brief 保持既有多态析构契约。
    virtual ~NoteMetadata() = default;

    /// @brief 按来源和字段名称保存原始文本，供格式导入导出往返使用。
    std::unordered_map<NoteMetadataType,
                       std::unordered_map<std::string, std::string, StringHash,
                                          std::equal_to<>>>
        note_properties;

    /// @brief 读取并转换指定来源的物件属性。
    /// @param source 扩展属性所属来源格式。
    /// @param key 来源域内的字段名称。
    /// @param default_value 来源或字段不存在时返回的默认值。
    /// @return 属性缺失时返回调用方给出的默认值。
    /// @note 数值转换继续使用原有流提取语义，不在依赖拆分中改变失败处理。
    template<typename T>
    T get_value(NoteMetadataType source, const std::string& key,
                T default_value = T())
    {
        // 来源与属性名分层保存，避免不同格式使用同名字段时相互覆盖。
        auto properties_it = note_properties.find(source);
        if ( properties_it == note_properties.end() ) return default_value;
        auto key_it = properties_it->second.find(key);
        if ( key_it == properties_it->second.end() ) return default_value;
        if constexpr ( std::is_same_v<T, std::string> ) {
            // 类型为字符串时整个返回
            return key_it->second;
        } else {
            // 直接从保存文本执行既有模板转换，不增加缓存或间接存储。
            std::istringstream iss(key_it->second);
            T                  value;
            iss >> value;
            return value;
        }
    }
};

}  // namespace MMM
