#pragma once

#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/SafeParse.h"
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
    /*
     * 0~4字节:int32 谱面时长
     * 5~8字节:int32 图时间点数
     *
     * 接下来每12字节按4字节int32+8字节float64(double)组合为一个时间点
     * 共${图时间点数}组timing数据
     *
     * 然后一个03 03未知意义的int16
     *
     * 接下来一个int32:表格行数
     *
     * 后面全是物件的数据
     * 11字节为一组
     * 00   00   00 00 00 00  00    00 00 00 00
     * 类型 没用    时间戳    轨道     参数
     * ---类型
     * 高位为0时不处于组合键中
     * 高位为6时处于组合键的第一个子键
     * 高位为2时处于组合键的中间部分子键
     * 高位为A时处于组合键的最后一个子键
     *
     * 低位为0时为单键类型
     * 低位为1时为滑键类型
     * 低位为2时为长键类型
     *
     * ---参数
     * 类型为长键时代表持续时间
     * 类型为滑键时为滑动参数
     *
     * 滑动参数为-1代表向左滑动1轨道
     * 滑动参数为-2代表向左滑动2轨道
     * 滑动参数为3代表向右滑动3轨道
     */

    // 创建谱面
    BeatMap beatMap;

    // 获取谱面基本元数据
    BaseMapMeta& basemeta = beatMap.m_baseMapMetadata;
    basemeta.map_path     = path;
    // 打开文件，以二进制模式读取
    // 切换为绝对路径
    if ( basemeta.map_path.is_relative() ) {
        basemeta.map_path = std::filesystem::absolute(basemeta.map_path);
    }
    // 将 path 转为 UTF-8 std::string 供日志使用
    auto pathToStr = [](const std::filesystem::path& p) {
        auto u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.c_str()),
                           u8.size());
    };
    XINFO("加载rm谱面路径:{}", pathToStr(basemeta.map_path));

    auto strToPath = [](const std::string& s) {
        return std::filesystem::path(
            reinterpret_cast<const char8_t*>(s.c_str()));
    };

    // 文件名
    auto fnamestr = pathToStr(basemeta.map_path.filename());
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

        if ( end_of_num != std::string::npos ) {
            try {
                std::string track_str =
                    fnamestr.substr(first_pos + 1, end_of_num - first_pos - 1);
                basemeta.track_count = MMM::Internal::safeStoi(track_str);
            } catch ( std::exception& e ) {
                XWARN("读取文件名轨道数失败: {}", e.what());
            }
        }
    }

    // 文件名最后一个点的位置
    auto last_pos = fnamestr.rfind(".");

    // 截取第二个_到最后一个.之间的字符串作为版本
    basemeta.version =
        (second_pos != std::string::npos && second_pos < last_pos)
            ? fnamestr.substr(second_pos + 1, last_pos - second_pos - 1)
            : "unknown";

    // 截取头到第一个_之间的字符串 作为音频文件的前缀-标题
    auto file_presuffix    = fnamestr.substr(0, first_pos);
    basemeta.title_unicode = file_presuffix;

    // 同文件夹内查询可能存在的音频文件
    bool has_audio{ true };
    // 前缀+.mp3作为音频文件名
    basemeta.main_audio_path =
        basemeta.map_path.parent_path() / strToPath(file_presuffix + ".mp3");
    // 也可为wav,ogg
    if ( !std::filesystem::exists(basemeta.main_audio_path) ) {
        basemeta.main_audio_path = basemeta.map_path.parent_path() /
                                   strToPath(file_presuffix + ".wav");
        if ( !std::filesystem::exists(basemeta.main_audio_path) ) {
            basemeta.main_audio_path = basemeta.map_path.parent_path() /
                                       strToPath(file_presuffix + ".ogg");
            if ( !std::filesystem::exists(basemeta.main_audio_path) ) {
                has_audio = false;
            }
        }
    }
    if ( !has_audio ) {
        basemeta.main_audio_path.clear();
        XWARN("未找到imd对应音频文件");
    }

    // 同文件夹内查询可能存在的封面文件
    // 检查前缀+.png 或.jpg .jpeg有哪个用哪个作为bg
    bool has_bg{ true };
    basemeta.main_cover_path =
        basemeta.map_path.parent_path() / strToPath(file_presuffix + ".png");
    if ( !std::filesystem::exists(basemeta.main_cover_path) ) {
        basemeta.main_cover_path = basemeta.map_path.parent_path() /
                                   strToPath(file_presuffix + ".jpg");
        if ( !std::filesystem::exists(basemeta.main_cover_path) ) {
            basemeta.main_cover_path = basemeta.map_path.parent_path() /
                                       strToPath(file_presuffix + ".jpeg");
            if ( !std::filesystem::exists(basemeta.main_cover_path) ) {
                has_bg = false;
            }
        }
    }
    if ( !has_bg ) {
        basemeta.main_cover_path.clear();
        XWARN("未找到imd对应背景图片");
    }

    // 实际执行二进制imd文件读取
    std::ifstream file(basemeta.map_path, std::ios::binary);

    if ( !file ) {
        XWARN("无法打开imd文件" + basemeta.map_path.generic_string());
        return {};
    }

    // 获取文件大小
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // 读取整个文件到vector中
    // 读取的全部数据
    std::vector<char> buffer_data(fileSize);
    file.read(buffer_data.data(), fileSize);

    // 创建二进制读取器
    BinaryReader reader;

    /// 当前读取到的字节位置指针
    auto data_pos = buffer_data.data();

    // 读取0~4字节:int32 谱面时长
    // 谱面时长
    basemeta.map_length = reader.read_value<int32_t>(data_pos);
    data_pos += 4;
    XINFO("谱面时长:[" + std::to_string(basemeta.map_length) + "]");

    // 读取5~8字节:int32 图时间线数量
    auto timing_point_amount =
        reader.read_value<int32_t>(buffer_data.data() + 4);
    data_pos += 4;
    XINFO("读取到imd文件时间线数量:[" + std::to_string(timing_point_amount) +
          "]");

    // 接下来每12字节按4字节int32+8字节float64(double)组合为一个时间点
    // 共${图时间点数}组timing数据
    std::vector<Timing> temp_timings;

    // 静态参考timing
    static Timing referenceTiming;

    // rm的timing不能变速,只能变bpm写谱--(附:ivm没写)
    for ( int i = 0; i < timing_point_amount; i++ ) {
        // 读取4字节:int32 时间线的时间戳位置
        auto timing_timestamp = reader.read_value<int32_t>(data_pos);
        data_pos += 4;
        // 读取8字节:double 时间线的bpm值
        auto timing_bpm = reader.read_value<double>(data_pos);
        data_pos += 8;
        Timing read_timing;
        read_timing.m_timestamp             = timing_timestamp;
        read_timing.m_bpm                   = timing_bpm;
        read_timing.m_beat_length           = 60000. / timing_bpm;
        read_timing.m_timingEffect          = TimingEffect::BPM;
        read_timing.m_timingEffectParameter = timing_bpm;

        // 防止ivm生成的一万个重复timing
        // 在这里，我们将所有不同的 timing 点加入表中
        if ( beatMap.m_timings.empty() ||
             read_timing.m_bpm != referenceTiming.m_bpm ) {
            referenceTiming = read_timing;
            XINFO("读取到timing:[time:" +
                  std::to_string(read_timing.m_timestamp) +
                  ",bpm:" + std::to_string(read_timing.m_bpm) + "]");
            // 加入谱面timing表
            if ( beatMap.m_timings.empty() ) {
                basemeta.preference_bpm = read_timing.m_bpm;
            }
            beatMap.m_timings.push_back(read_timing);
        }
    }

    // 然后一个03 03未知意义的int16
    // 跳过这2字节
    data_pos += 2;

    // 读取一个int32: 表格行数
    auto table_rows = reader.read_value<int32_t>(data_pos);
    data_pos += 4;
    XINFO("读取到表格行数:[" + std::to_string(table_rows) + "]");

    // 后面全是物件的数据
    // 11字节为一组
    // 00   00   00 00 00 00  00    00 00 00 00
    // 类型 没用    时间戳    轨道     参数

    // 缓存note父类指针
    Note* temp_note_ptr{ nullptr };
    // 当前折线物件 (这里不需要 static，每次 new 一个干净的更好，避免状态残留)
    Polyline current_polyline;
    bool     is_building_polyline = false;  // 是否正在构建折线
    // 静态折线物件
    static Polyline polyline;
    polyline.m_timestamp = -1e-17;
    polyline.m_type      = NoteType::POLYLINE;
    // 折线物件是否构建完成
    bool polyline_done{ true };
    // 物件总数(折线物件中的子物件也算作一个物件)
    int obj_count{ 0 };

    // 读取全部物件 (读到文件结束)
    while ( (buffer_data.data() + buffer_data.size() - data_pos) > 0 ) {
        // 类型1字节
        auto note_type_info = reader.read_value<int8_t>(data_pos);
        // 高位为折线类型信息
        auto note_complex_info = note_type_info & 0xf0;
        // 低位为物件本身类型信息
        auto note_type = note_type_info & 0x0f;
        // 然后有一个固定没用的 00 单字节

        // 移动2字节
        data_pos += 2;

        // 时间戳4字节int32
        auto note_timestamp = reader.read_value<int32_t>(data_pos);
        data_pos += 4;

        // 轨道位置1字节uchar
        auto note_track = reader.read_value<uint8_t>(data_pos);
        data_pos += 1;

        // 物件参数4字节int32
        auto note_parameter = reader.read_value<int32_t>(data_pos);
        data_pos += 4;

        // 更新谱面可能的最大轨道数
        if ( note_track + 1 > basemeta.track_count )
            basemeta.track_count = note_track + 1;

        // 更新谱面可能的最大长度
        if ( int64_t(note_timestamp) > basemeta.map_length )
            basemeta.map_length = note_timestamp;

        // 初始化物件
        switch ( note_type ) {
        case 0: {
            // 是单键Note
            // 构造一个物件填入数据
            Note& note = beatMap.m_noteData.notes.emplace_back();
            // 取物件指针方便后续更新相同物件属性
            temp_note_ptr = &note;
            break;
        }
        case 1: {
            // 是滑键Flick
            // 构造一个Flick填入数据
            Flick& flick = beatMap.m_noteData.flicks.emplace_back();
            flick.m_type = NoteType::FLICK;
            // 对于Flick来说note_parameter就是m_dtrack
            flick.m_dtrack = note_parameter;
            // 取物件指针方便后续更新相同物件属性
            temp_note_ptr = &flick;
            break;
        }
        case 2: {
            // 是长条Hold
            // 构造一个Hold填入数据
            Hold& hold  = beatMap.m_noteData.holds.emplace_back();
            hold.m_type = NoteType::HOLD;
            // 对于Hold来说note_parameter就是m_duration
            hold.m_duration = note_parameter;
            // 取物件指针方便后续更新相同物件属性
            temp_note_ptr = &hold;

            // 读取到Hold可能需要再更新谱面长度
            if ( auto hold_end = hold.m_timestamp + hold.m_duration;
                 hold_end > basemeta.map_length )
                basemeta.map_length = hold_end;
            break;
        }
        }

        // 更新本物件共同属性
        temp_note_ptr->m_timestamp = note_timestamp;
        temp_note_ptr->m_metadata.note_properties[NoteMetadataType::RM]["Parameter"] = std::to_string(note_parameter);
        temp_note_ptr->m_track     = note_track;
        // 同时加入到全局引用表中
        beatMap.m_allNotes.push_back(*temp_note_ptr);

        // 处理折线逻辑
        switch ( note_complex_info ) {
        case 0x60: {  // 折线头
            current_polyline.m_timestamp = temp_note_ptr->m_timestamp;
            current_polyline.m_track     = temp_note_ptr->m_track;
            current_polyline.m_type      = NoteType::POLYLINE;
            is_building_polyline         = true;
            break;
        }
        case 0x20: {                                // 折线中
            if ( !is_building_polyline ) continue;  // 容错处理
            break;
        }
        case 0xa0: {                                // 折线尾
            if ( !is_building_polyline ) continue;  // 容错处理
            break;
        }
        }

        // 如果当前处于构建折线的状态，添加引用到当前折线中
        if ( is_building_polyline ) {
            current_polyline.m_subNotes.push_back(*temp_note_ptr);

            switch ( temp_note_ptr->m_type ) {
            case NoteType::FLICK: {
                current_polyline.m_subFlicks.push_back(
                    *static_cast<Flick*>(temp_note_ptr));
                break;
            }
            case NoteType::HOLD: {
                current_polyline.m_subHolds.push_back(
                    *static_cast<Hold*>(temp_note_ptr));
                break;
            }
            default: break;
            }
        }

        // 如果遇到了折线尾，说明本条折线结束，可以打包塞进去了
        if ( note_complex_info == 0xa0 && is_building_polyline ) {
            beatMap.m_noteData.polylines.push_back(current_polyline);
            beatMap.m_allNotes.push_back(beatMap.m_noteData.polylines.back());

            // 清空 current_polyline，准备迎接下一个组合键
            current_polyline.m_subFlicks.clear();
            current_polyline.m_subHolds.clear();
            current_polyline.m_subNotes.clear();
            current_polyline.m_metadata.note_properties.clear();
            is_building_polyline = false;
        }

        ++obj_count;
    }

    // orbits = max_orbits;
    XINFO("读取到物件:" + std::to_string(obj_count));
    // qDebug() << "table_rows:" << std::to_string(table_rows);

    // 生成图名
    basemeta.name = std::format("[rm] {} [{}k] {}",
                                file_presuffix,
                                std::to_string(basemeta.track_count),
                                basemeta.version);

    return beatMap;
}

}  // namespace MMM
