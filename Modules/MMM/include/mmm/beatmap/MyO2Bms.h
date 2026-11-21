#pragma once

#include "mmm/metadata/MapMetadata.h"

#include <filesystem>
#include <string_view>

namespace MMM::Bms
{
/// @brief 读取 BMS 专用属性，不为缺失字段创建默认条目。
/// @param mapMetadata 源谱面或导出快照持有的元数据。
/// @param key BMS 属性的稳定键名。
/// @return 借用属性值；缺失时为空，修改属性表前应消费或复制结果。
inline std::string_view bmsProperty(const MapMetadata& mapMetadata,
                                    std::string_view   key)
{
    const auto format = mapMetadata.map_properties.find(MapMetadataType::BMS);
    if ( format == mapMetadata.map_properties.end() ) return {};
    const auto value = format->second.find(key);
    return value == format->second.end() ? std::string_view{} : value->second;
}

/// @brief 判断是否按 MyO2 扩展协议输出当前 BMS 快照。
/// @details 兼容吸附隐含扩展协议；导入的 MyO2 来源也可按原精度直接保存。
/// 普通另存为和打包可显式关闭来源协议，避免静默改变现有标准 BMS 输出。
inline bool usesMyO2Format(const MapMetadata& mapMetadata)
{
    return bmsProperty(mapMetadata, "myo2_grid") == "1" ||
           bmsProperty(mapMetadata, "myo2_format") == "1";
}

/// @brief 历史模式的整曲音频，空值交给导出器根据音轨选用模式。
inline constexpr std::string_view MYO2_AUDIO_MODE = "myo2_audio_mode";
/// @brief 可选的现有 KEY 音 ZIP 路径；其 WAV 名称相对 ZIP 内部根目录。
inline constexpr std::string_view MYO2_KEYSOUND = "myo2_keysound";
/// @brief 变长小节的属性前缀，后缀为十进制小节编号，值为四拍比例。
inline constexpr std::string_view MYO2_MEASURE_PREFIX = "myo2_measure_";
/// @brief 控制事件的属性前缀，按序号存储，值为标记码及谱面毫秒时间。
inline constexpr std::string_view MYO2_CUE_PREFIX = "myo2_cue_";
/// @brief 打包首拍对齐对未建模控制标记施加的共同毫秒位移。
inline constexpr std::string_view MYO2_CUE_SHIFT = "myo2_cue_shift_ms";

/// @brief 为 STANDARD 静音物件定位浏览器可以实际解码的 WAV。
/// @details 同目录的谱面共用一份固定的静音内容，资源包按目录去重。
/// 文件名使用固定 ASCII 路径，避免经过歌曲库转 XML 时改变引用。
inline std::filesystem::path myO2SilencePath(const std::filesystem::path& chart)
{
    return chart.parent_path() / "__mmm_myo2_silence.wav";
}
}  // namespace MMM::Bms
