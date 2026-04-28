#pragma once

#include <filesystem>
#include <sstream>
#include <string>
#include <unordered_map>

namespace MMM
{
struct StringHash {
    // 这个标签用于开启透明性
    using is_transparent = void;
    [[nodiscard]] size_t operator()(const char* txt) const
    {
        return std::hash<std::string_view>{}(txt);
    }
    [[nodiscard]] size_t operator()(std::string_view txt) const
    {
        return std::hash<std::string_view>{}(txt);
    }
    [[nodiscard]] size_t operator()(const std::string& txt) const
    {
        return std::hash<std::string>{}(txt);
    }
};

enum class MapMetadataType {
    OSU,
    MALODY,
    RM,
};

class MapMetadata
{
public:
    // 构造MapMetadata
    MapMetadata() = default;
    // 析构MapMetadata
    virtual ~MapMetadata() = default;

    // 统一通用属性表(来源-[属性名-属性值])
    std::unordered_map<MapMetadataType,
                       std::unordered_map<std::string, std::string, StringHash,
                                          std::equal_to<>>>
        map_properties;

    // 获取数据
    template<typename T>
    T get_value(MapMetadataType source, const std::string& key,
                T default_value = T())
    {
        auto properties_it = map_properties.find(source);
        if ( properties_it == map_properties.end() ) return default_value;
        auto key_it = properties_it->second.find(key);
        if ( key_it == properties_it->second.end() ) return default_value;
        if constexpr ( std::is_same_v<T, std::string> ) {
            // 类型为字符串时整个返回
            return key_it->second;
        } else {
            std::istringstream iss(key_it->second);
            T                  value;
            iss >> value;
            return value;
        }
    }
};

enum class CoverType {
    IMAGE,
    VIDEO,
};

// 基本谱面信息
struct BaseMapMeta {
    // 谱面名称
    std::string name;
    // 谱面歌曲标题
    std::string title;
    // 谱面歌曲标题(unicode)
    std::string title_unicode;
    // 谱面歌曲艺术家
    std::string artist;
    // 谱面歌曲艺术家(unicode)
    std::string artist_unicode;
    // 谱面文件路径
    std::filesystem::path map_path;
    // 主音频文件路径
    std::filesystem::path main_audio_path;
    // 主背景文件路径
    std::filesystem::path main_cover_path;
    // 主背景类型
    CoverType cover_type{ CoverType::IMAGE };
    // 主背景类型为视频时的开始时间
    int32_t video_starttime{ 0 };
    // 背景的位置x偏移
    int32_t bgxoffset{ 0 };
    // 背景的位置y偏移
    int32_t bgyoffset{ 0 };

    // 谱面版本名
    std::string version;
    // 谱面作者
    std::string author;

    // 谱面参考bpm
    double preference_bpm{ -1. };
    // 谱面轨道数
    int32_t track_count{ -1 };
    // 谱面总时长
    double map_length{ -1. };
};

enum class NoteMetadataType {
    OSU,
    MALODY,
    RM,
};

class NoteMetadata
{
public:
    // 构造NoteMetadata
    NoteMetadata() = default;
    // 析构NoteMapMetadata
    virtual ~NoteMetadata() = default;

    // 统一通用属性表(来源-[属性名-属性值])
    std::unordered_map<NoteMetadataType,
                       std::unordered_map<std::string, std::string, StringHash,
                                          std::equal_to<>>>
        note_properties;

    // 获取数据
    template<typename T>
    T get_value(NoteMetadataType source, const std::string& key,
                T default_value = T())
    {
        auto properties_it = note_properties.find(source);
        if ( properties_it == note_properties.end() ) return default_value;
        auto key_it = properties_it->second.find(key);
        if ( key_it == properties_it->second.end() ) return default_value;
        if constexpr ( std::is_same_v<T, std::string> ) {
            // 类型为字符串时整个返回
            return key_it->second;
        } else {
            std::istringstream iss(key_it->second);
            T                  value;
            iss >> value;
            return value;
        }
    }
};

enum class TimingMetadataType {
    OSU,
    RM,
    MALODY,
};

class TimingMetadata
{
public:
    // 构造NoteMetadata
    TimingMetadata() = default;
    // 析构NoteMapMetadata
    virtual ~TimingMetadata() = default;

    // 元数据类型

    // 属性表
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
