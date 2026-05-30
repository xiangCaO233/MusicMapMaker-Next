#pragma once

#include "log/colorful-log.h"
#include "mmm/SafeParse.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Polyline.h"
#include <cstring>
#include <filesystem>
#include <fstream>

namespace MMM
{

// 二进制读取器
class BinaryReader
{
public:
    // 读取指定指针位置的数据
    template<typename T>
    T read_value(const char* data, bool is_little_endian = true)
    {
        T value;
        std::memcpy(&value, data, sizeof(T));

        if ( !is_little_endian ) {
            char* ptr = reinterpret_cast<char*>(&value);
            std::reverse(ptr, ptr + sizeof(T));
        }

        return value;
    }
};

inline BeatMap loadRMMap(std::filesystem::path path)
{
    // 创建谱面
    BeatMap beatMap;

    // 获取谱面基本元数据
    BaseMapMeta& basemeta = beatMap.m_baseMapMetadata;
    basemeta.map_path     = path;

    // 切换为绝对路径 (使用 error_code 避免异常)
    std::error_code ec;
    if ( basemeta.map_path.is_relative() ) {
        auto abs_path = std::filesystem::absolute(basemeta.map_path, ec);
        if ( !ec ) {
            basemeta.map_path = abs_path;
        }
    }

    XINFO("加载rm谱面路径:{}", Config::pathToUtf8(basemeta.map_path));

    // 文件名
    std::string fnamestr = Config::pathToUtf8(basemeta.map_path.filename());
    // 文件名第一个下划线的位置 (Title_nk_Version.imd)
    auto first_pos = fnamestr.find('_');
    // 文件名第二个下划线的位置
    auto second_pos = fnamestr.find(
        '_', (first_pos == std::string::npos) ? 0 : first_pos + 1);

    // 读取文件名中的轨道数 (nk)
    if ( first_pos != std::string::npos ) {
        // 尝试寻找 'k'，因为格式是 nk (如 4k, 10k)
        auto k_pos = fnamestr.find('k', first_pos + 1);
        // 如果有 k 且在第二个下划线之前（或没有第二个下划线）
        size_t end_of_num =
            (k_pos != std::string::npos &&
             (second_pos == std::string::npos || k_pos < second_pos))
                ? k_pos
                : second_pos;

        if ( end_of_num != std::string::npos && end_of_num > first_pos + 1 ) {
            try {
                std::string track_str =
                    fnamestr.substr(first_pos + 1, end_of_num - first_pos - 1);
                basemeta.track_count = MMM::Internal::safeStoi(track_str);
            } catch ( ... ) {
                XWARN("读取文件名轨道数失败: {}", fnamestr);
            }
        }
    }

    // 文件名最后一个点的位置
    auto last_pos = fnamestr.rfind(".");

    // 截取第二个_到最后一个.之间的字符串作为版本
    basemeta.version =
        (second_pos != std::string::npos && last_pos != std::string::npos &&
         second_pos < last_pos)
            ? fnamestr.substr(second_pos + 1, last_pos - second_pos - 1)
            : "unknown";

    // 截取头到第一个_之间的字符串 作为音频文件的前缀-标题
    std::string file_presuffix =
        (first_pos != std::string::npos) ? fnamestr.substr(0, first_pos) : "";
    basemeta.title_unicode = file_presuffix;

    // 同文件夹内查询可能存在的音频文件
    if ( !file_presuffix.empty() ) {
        auto parent = basemeta.map_path.parent_path();
        bool has_audio{ true };
        basemeta.main_audio_path = Config::utf8ToPath(file_presuffix + ".mp3");
        if ( !std::filesystem::exists(parent / basemeta.main_audio_path, ec) ) {
            basemeta.main_audio_path =
                Config::utf8ToPath(file_presuffix + ".wav");
            if ( !std::filesystem::exists(parent / basemeta.main_audio_path,
                                          ec) ) {
                basemeta.main_audio_path =
                    Config::utf8ToPath(file_presuffix + ".ogg");
                if ( !std::filesystem::exists(parent / basemeta.main_audio_path,
                                              ec) ) {
                    has_audio = false;
                }
            }
        }
        if ( !has_audio ) {
            basemeta.main_audio_path.clear();
            XWARN("未找到imd对应音频文件");
        }

        // 同文件夹内查询可能存在的封面文件
        bool has_bg{ true };
        basemeta.main_cover_path = Config::utf8ToPath(file_presuffix + ".png");
        if ( !std::filesystem::exists(parent / basemeta.main_cover_path, ec) ) {
            basemeta.main_cover_path =
                Config::utf8ToPath(file_presuffix + ".jpg");
            if ( !std::filesystem::exists(parent / basemeta.main_cover_path,
                                          ec) ) {
                basemeta.main_cover_path =
                    Config::utf8ToPath(file_presuffix + ".jpeg");
                if ( !std::filesystem::exists(parent / basemeta.main_cover_path,
                                              ec) ) {
                    has_bg = false;
                }
            }
        }
        if ( !has_bg ) {
            basemeta.main_cover_path.clear();
            XWARN("未找到imd对应背景图片");
        }
    }

    // 实际执行二进制imd文件读取
    std::ifstream file(basemeta.map_path, std::ios::binary);
    if ( !file ) {
        XWARN("无法打开imd文件: {}", Config::pathToUtf8(basemeta.map_path));
        return {};
    }

    // 获取文件大小
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    if ( fileSize < 8 ) {
        XWARN("imd文件太小，格式不合法: {}",
              Config::pathToUtf8(basemeta.map_path));
        return {};
    }

    // 读取整个文件到vector中
    std::vector<char> buffer_data(fileSize);
    if ( !file.read(buffer_data.data(), fileSize) ) {
        XWARN("读取imd文件数据失败: {}", Config::pathToUtf8(basemeta.map_path));
        return {};
    }

    // 创建二进制读取器
    BinaryReader reader;
    const char*  data_start = buffer_data.data();
    const char*  data_end   = data_start + fileSize;
    const char*  data_pos   = data_start;

    // 安全检查宏
#define CHECK_BOUNDS(needed)                 \
    if ( data_pos + (needed) > data_end ) {  \
        XERROR("imd文件意外结束，读取失败"); \
        return beatMap;                      \
    }

    // 读取0~4字节:int32 谱面时长
    CHECK_BOUNDS(4);
    basemeta.map_length = reader.read_value<int32_t>(data_pos);
    auto& rmMapProps = beatMap.m_metadata.map_properties[MapMetadataType::RM];
    rmMapProps["mapLength"] =
        std::to_string(static_cast<int32_t>(basemeta.map_length));
    data_pos += 4;
    XINFO("谱面时长: {}", basemeta.map_length);

    // 读取5~8字节:int32 图时间线数量
    CHECK_BOUNDS(4);
    auto timing_point_amount = reader.read_value<int32_t>(data_pos);
    data_pos += 4;
    XINFO("读取到imd文件时间线数量: {}", timing_point_amount);

    // 接下来每12字节按4字节int32+8字节float64(double)组合为一个时间点
    Timing referenceTiming;
    referenceTiming.m_bpm = -1.0;

    for ( int i = 0; i < timing_point_amount; i++ ) {
        CHECK_BOUNDS(12);
        auto timing_timestamp = reader.read_value<int32_t>(data_pos);
        data_pos += 4;
        auto timing_bpm = reader.read_value<double>(data_pos);
        data_pos += 8;

        Timing read_timing;
        read_timing.m_timestamp    = timing_timestamp;
        read_timing.m_bpm          = timing_bpm;
        read_timing.m_beat_length  = 60000. / std::max(0.001, timing_bpm);
        read_timing.m_timingEffect = TimingEffect::BPM;
        read_timing.m_timingEffectParameter = timing_bpm;

        if ( beatMap.m_timings.empty() ||
             std::abs(read_timing.m_bpm - referenceTiming.m_bpm) > 0.0001 ) {
            referenceTiming = read_timing;
            if ( beatMap.m_timings.empty() ) {
                basemeta.preference_bpm = read_timing.m_bpm;
            }
            beatMap.m_timings.push_back(read_timing);
        }
    }

    // 然后一个03 03未知意义的int16
    CHECK_BOUNDS(2);
    data_pos += 2;

    // 读取一个int32: 表格行数
    CHECK_BOUNDS(4);
    auto table_rows       = reader.read_value<int32_t>(data_pos);
    rmMapProps["tabRows"] = std::to_string(table_rows);
    data_pos += 4;
    XINFO("读取到表格行数: {}", table_rows);

    // 缓存note父类指针
    Note*    temp_note_ptr{ nullptr };
    Polyline current_polyline;
    bool     is_building_polyline = false;
    int      obj_count{ 0 };

    std::vector<std::vector<std::pair<NoteType, size_t>>> all_polylines_indices;
    std::vector<std::pair<NoteType, size_t>> current_polyline_indices;

    // 读取全部物件
    while ( data_pos < data_end ) {
        CHECK_BOUNDS(11);
        auto note_type_info    = reader.read_value<int8_t>(data_pos);
        auto note_complex_info = note_type_info & 0xf0;
        auto note_type         = note_type_info & 0x0f;
        data_pos += 2;  // 跳过类型和固定位

        auto note_timestamp = reader.read_value<int32_t>(data_pos);
        data_pos += 4;

        auto note_track = reader.read_value<uint8_t>(data_pos);
        data_pos += 1;

        auto note_parameter = reader.read_value<int32_t>(data_pos);
        data_pos += 4;

        // 更新元数据
        basemeta.track_count =
            std::max(basemeta.track_count, (int32_t)note_track + 1);
        basemeta.map_length =
            std::max((int64_t)basemeta.map_length, (int64_t)note_timestamp);

        // 初始化物件
        switch ( note_type ) {
        case 0: {  // Note
            Note& note    = beatMap.m_noteData.notes.emplace_back();
            temp_note_ptr = &note;
            break;
        }
        case 1: {  // Flick
            Flick& flick   = beatMap.m_noteData.flicks.emplace_back();
            flick.m_type   = NoteType::FLICK;
            flick.m_dtrack = note_parameter;
            temp_note_ptr  = &flick;
            break;
        }
        case 2: {  // Hold
            Hold& hold      = beatMap.m_noteData.holds.emplace_back();
            hold.m_type     = NoteType::HOLD;
            hold.m_duration = note_parameter;
            temp_note_ptr   = &hold;
            basemeta.map_length =
                std::max((int64_t)basemeta.map_length,
                         (int64_t)note_timestamp + note_parameter);
            break;
        }
        default: continue;
        }

        temp_note_ptr->m_timestamp = note_timestamp;
        temp_note_ptr->m_track     = note_track;
        temp_note_ptr->m_metadata
            .note_properties[NoteMetadataType::RM]["Parameter"] =
            std::to_string(note_parameter);

        // 处理折线逻辑
        if ( note_complex_info == 0x60 ) {  // 头
            current_polyline.m_timestamp = temp_note_ptr->m_timestamp;
            current_polyline.m_track     = temp_note_ptr->m_track;
            current_polyline.m_type      = NoteType::POLYLINE;
            current_polyline.m_subNotes.clear();
            current_polyline.m_subFlicks.clear();
            current_polyline.m_subHolds.clear();
            current_polyline_indices.clear();
            is_building_polyline = true;
        }

        if ( is_building_polyline ) {
            size_t idx = 0;
            if ( temp_note_ptr->m_type == NoteType::NOTE ) {
                idx = beatMap.m_noteData.notes.size() - 1;
            } else if ( temp_note_ptr->m_type == NoteType::FLICK ) {
                idx = beatMap.m_noteData.flicks.size() - 1;
            } else if ( temp_note_ptr->m_type == NoteType::HOLD ) {
                idx = beatMap.m_noteData.holds.size() - 1;
            }
            current_polyline_indices.push_back({ temp_note_ptr->m_type, idx });

            if ( note_complex_info == 0xa0 ) {  // 尾
                beatMap.m_noteData.polylines.push_back(current_polyline);
                all_polylines_indices.push_back(current_polyline_indices);
                is_building_polyline = false;
            }
        }
        obj_count++;
    }

#undef CHECK_BOUNDS

    XINFO("读取到物件总数: {}", obj_count);

    // 重新构建所有折线子物件的引用 (解析最终的稳定内存地址)
    for ( size_t i = 0; i < beatMap.m_noteData.polylines.size(); ++i ) {
        auto&       poly    = beatMap.m_noteData.polylines[i];
        const auto& indices = all_polylines_indices[i];
        for ( const auto& [type, idx] : indices ) {
            if ( type == NoteType::NOTE ) {
                auto& ref = beatMap.m_noteData.notes[idx];
                poly.m_subNotes.push_back(std::ref(ref));
            } else if ( type == NoteType::HOLD ) {
                auto& ref = beatMap.m_noteData.holds[idx];
                poly.m_subNotes.push_back(std::ref(ref));
                poly.m_subHolds.push_back(std::ref(ref));
            } else if ( type == NoteType::FLICK ) {
                auto& ref = beatMap.m_noteData.flicks[idx];
                poly.m_subNotes.push_back(std::ref(ref));
                poly.m_subFlicks.push_back(std::ref(ref));
            }
        }
    }

    beatMap.sync();

    basemeta.name =
        std::format("[rm] {} [{}k] {}",
                    (file_presuffix.empty() ? "Map" : file_presuffix),
                    basemeta.track_count,
                    basemeta.version);

    return beatMap;
}

}  // namespace MMM
