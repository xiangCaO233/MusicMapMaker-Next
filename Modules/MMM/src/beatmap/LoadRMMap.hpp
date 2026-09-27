#pragma once

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/SafeParse.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Polyline.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <string>

namespace MMM
{

/// @file LoadRMMap.hpp
/// @brief RM/IMD 二进制谱面的兼容读取实现。
///
/// 文件体布局按顺序固定如下：
/// - `int32 mapLength`：谱面声明时长，单位为毫秒；
/// - `int32 timingCount`：BPM 记录数量；
/// - `timingCount * (int32 timestamp + double bpm)`：节奏表；
/// - `int16 0x0303`：历史固定标记；
/// - `int32 tableRows`：物件表声明行数；
/// - 直到文件尾的 11 字节物件记录。
///
/// 每条物件记录依次包含：
/// - `int8 typeInfo`：高四位为折线位置，低四位为基础类型；
/// - `int8 reserved`：保留字节；
/// - `int32 timestamp`：物件起始时间；
/// - `uint8 track`：玩家轨道；
/// - `int32 parameter`：依基础类型解释的参数。
///
/// `typeInfo` 的低四位语义为：
/// - `0`：普通 Note，parameter 作为未知兼容字段保留；
/// - `1`：Flick，parameter 表示目标轨道偏移；
/// - `2`：Hold，parameter 表示持续时间。
///
/// `typeInfo` 的高四位语义为：
/// - `0x00`：独立物件；
/// - `0x60`：折线首节点；
/// - `0x20`：折线中间节点；
/// - `0xA0`：折线尾节点。
///
/// 文件名也是格式契约的一部分。标准形式 `Title_nk_Version.imd` 提供标题、
/// 玩家轨道数和难度版本；同目录下 `Title.<音频扩展名>` 与
/// `Title.png/jpg/jpeg` 分别作为主音频和封面。文件体本身不保存这些路径。
///
/// 解析器维持以下不变量：
/// - 任何定宽字段读取之前都先检查剩余字节；
/// - 文件名解析失败不影响已知文件体内容的读取；
/// - 未知基础物件类型不会构造无效的统一模型对象；
/// - 折线节点先记录类型容器索引，容器停止增长后才建立引用；
/// - 派生索引只在所有拥有容器和折线引用完整后同步；
/// - 伴随资源缺失只清空资源路径，不使谱面主体加载失败。
/// - 原格式专属字段保存在对应 Metadata 命名空间中；
/// - 展示名称最后生成，不参与任何资源或物件解析决策。

/**
 * @brief 读取 RM/IMD 文件中的定宽标量。
 *
 * RM/IMD 没有自描述字段，游标推进完全依赖格式规定的字段宽度。调用方必须
 * 先完成剩余字节检查，再把当前位置交给读取器；读取器只负责规避未对齐访问，
 * 不负责验证缓冲区边界。当前格式按小端存储，但保留反转分支便于解析已有的
 * 非标准样本。
 */
class BinaryReader
{
public:
    /// @brief 从指定字节位置复制一个标量值。
    /// @tparam T 可按原始字节复制的定宽标量类型。
    /// @param data 字段首字节，调用前必须确认至少剩余 sizeof(T) 字节。
    /// @param is_little_endian 输入是否采用主机所使用的小端顺序。
    /// @return 解码后的值。
    template<typename T>
    T read_value(const char* data, bool is_little_endian = true)
    {
        // memcpy 同时规避文件游标可能未自然对齐的问题。
        T value;
        std::memcpy(&value, data, sizeof(T));

        if ( !is_little_endian ) {
            // 字节反转只改变局部副本，不会修改原始输入缓冲区。
            char* ptr = reinterpret_cast<char*>(&value);
            std::reverse(ptr, ptr + sizeof(T));
        }

        return value;
    }
};

/// @brief 查找 RM/imd 谱面同名前缀的主音频文件。
/// @param parent 谱面所在目录。
/// @param filePrefix 音频文件名前缀。
/// @return 找到时返回相对音频文件名，否则返回空路径。
/// @note RM/IMD 不在文件体内记录音频路径，只能依据谱面文件名前缀推导。
inline std::filesystem::path resolveRMAudioPath(
    const std::filesystem::path& parent, const std::string& filePrefix)
{
    if ( filePrefix.empty() ) {
        return {};
    }

    // 扩展名顺序也是冲突时的选择优先级，保持稳定才能保证重复导入结果一致。
    static constexpr std::array<std::string_view, 7> AUDIO_EXTENSIONS{
        ".mp3", ".wav", ".ogg", ".flac", ".opus", ".aac", ".m4a"
    };

    // 文件探测属于兼容性补全；单个候选查询失败不应阻止继续尝试后续格式。
    std::error_code ec;
    for ( const auto extension : AUDIO_EXTENSIONS ) {
        auto candidate =
            Config::utf8ToPath(filePrefix + std::string(extension));
        if ( std::filesystem::exists(parent / candidate, ec) && !ec ) {
            return candidate;
        }
        ec.clear();
    }
    return {};
}

/// @brief 加载 RM/imd 谱面文件。
/// @param path 谱面文件路径。
/// @return 解析出的谱面数据。
/// @details 文件名承担标题、轨道数和难度版本等元数据，文件体依次包含谱面
/// 时长、BPM 时间点、固定标记、物件行数和定宽物件记录。解析器先保存原格式
/// 中可往返的字段，再把扁平物件流恢复成统一的 NoteData 与 Polyline 引用结构。
/// @note 格式损坏时返回已经安全解析的部分数据或空谱面，不越过输入缓冲区。
inline BeatMap loadRMMap(std::filesystem::path path)
{
    // 先建立返回对象，使截断文件也能携带已经确认的路径及元数据。
    BeatMap beatMap;

    // 获取谱面基本元数据
    BaseMapMeta& basemeta = beatMap.m_baseMapMetadata;
    basemeta.map_path     = path;

    // 路径改为绝对形式，后续伴随资源探测便不依赖进程当前工作目录。
    // 使用 error_code 保持解析入口无异常；转换失败时保留调用者传入的路径。
    std::error_code ec;
    if ( basemeta.map_path.is_relative() ) {
        auto abs_path = std::filesystem::absolute(basemeta.map_path, ec);
        if ( !ec ) {
            basemeta.map_path = abs_path;
        }
    }

    XINFO("加载rm谱面路径:{}", Config::pathToUtf8(basemeta.map_path));

    // 标准命名约定为 Title_nk_Version.imd。旧文件可能缺少部分分隔符，
    // 因此每一段都单独验证位置，并为无法推导的字段保留安全默认值。
    std::string fnamestr = Config::pathToUtf8(basemeta.map_path.filename());
    // 文件名第一个下划线的位置 (Title_nk_Version.imd)
    auto first_pos = fnamestr.find('_');
    // 文件名第二个下划线的位置
    auto second_pos = fnamestr.find(
        '_', (first_pos == std::string::npos) ? 0 : first_pos + 1);

    // 轨道数位于第一个下划线后的 nk 段；允许 10k 等多位轨道数。
    if ( first_pos != std::string::npos ) {
        // 优先以 k 结束数字，兼容缺少第二个下划线的旧式文件名。
        auto k_pos = fnamestr.find('k', first_pos + 1);
        // 如果有 k 且在第二个下划线之前（或没有第二个下划线）
        size_t end_of_num =
            (k_pos != std::string::npos &&
             (second_pos == std::string::npos || k_pos < second_pos))
                ? k_pos
                : second_pos;

        if ( end_of_num != std::string::npos && end_of_num > first_pos + 1 ) {
            std::string track_str =
                fnamestr.substr(first_pos + 1, end_of_num - first_pos - 1);
            // 非法数字不覆盖 BeatMap 默认轨道数，避免产生零轨或负轨谱面。
            const int parsedTrackCount = MMM::Internal::safeStoi(track_str, -1);
            if ( parsedTrackCount > 0 ) {
                basemeta.track_count = parsedTrackCount;
            } else {
                XWARN("读取文件名轨道数失败: {}", fnamestr);
            }
        }
    }

    // 文件名最后一个点的位置
    auto last_pos = fnamestr.rfind(".");

    // 难度版本只从完整的第二分隔段提取，不能把扩展名或标题误当作版本。
    basemeta.version =
        (second_pos != std::string::npos && last_pos != std::string::npos &&
         second_pos < last_pos)
            ? fnamestr.substr(second_pos + 1, last_pos - second_pos - 1)
            : "unknown";

    // 标题同时是伴随音频和封面的文件名前缀，这是 RM 格式的隐式资源约定。
    std::string file_presuffix =
        (first_pos != std::string::npos) ? fnamestr.substr(0, first_pos) : "";
    basemeta.title_unicode = file_presuffix;

    // 伴随资源只记录相对文件名，移动整个项目目录后仍可继续解析。
    if ( !file_presuffix.empty() ) {
        auto parent              = basemeta.map_path.parent_path();
        basemeta.main_audio_path = resolveRMAudioPath(parent, file_presuffix);
        if ( basemeta.main_audio_path.empty() ) {
            basemeta.main_audio_path.clear();
            XWARN("未找到imd对应音频文件");
        } else {
            basemeta.song_file_hint = basemeta.main_audio_path;
        }

        // 封面同样依赖前缀，并按 png、jpg、jpeg 的固定顺序探测。
        // 仅在文件实际存在时保留路径，避免下游把推测路径当作有效资源。
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

    // 下面进入文件体解析。只有完整读入内存后才建立首尾游标，从而让所有
    // 定宽字段共享同一套边界判断，不在多次流读取之间遗漏失败状态。
    std::ifstream file(basemeta.map_path, std::ios::binary);
    if ( !file ) {
        XWARN("无法打开imd文件: {}", Config::pathToUtf8(basemeta.map_path));
        return {};
    }

    // 文件头至少包含 mapLength 与 timingCount 两个 int32 字段。
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    if ( fileSize < 8 ) {
        XWARN("imd文件太小，格式不合法: {}",
              Config::pathToUtf8(basemeta.map_path));
        return {};
    }

    // 一次性读取也保证游标引用在整个解析周期内保持稳定。
    std::vector<char> buffer_data(fileSize);
    if ( !file.read(buffer_data.data(), fileSize) ) {
        XWARN("读取imd文件数据失败: {}", Config::pathToUtf8(basemeta.map_path));
        return {};
    }

    // data_pos 是唯一前进游标；data_end 是所有字段读取的硬边界。
    BinaryReader reader;
    const char*  data_start = buffer_data.data();
    const char*  data_end   = data_start + fileSize;
    const char*  data_pos   = data_start;

    // 每个字段组在解码前声明所需字节数。失败时返回已解析对象，既不越界，
    // 也不把未初始化内容写入模型。宏仅在本函数解析区间内可见。
#define CHECK_BOUNDS(needed)                 \
    if ( data_pos + (needed) > data_end ) {  \
        XERROR("imd文件意外结束，读取失败"); \
        return beatMap;                      \
    }

    // 首字段是谱面声明时长。原值同时写入 RM 扩展元数据，保存时可精确往返。
    CHECK_BOUNDS(4);
    basemeta.map_length = reader.read_value<int32_t>(data_pos);
    auto& rmMapProps = beatMap.m_metadata.map_properties[MapMetadataType::RM];
    rmMapProps["mapLength"] =
        std::to_string(static_cast<int32_t>(basemeta.map_length));
    data_pos += 4;
    XINFO("谱面时长: {}", basemeta.map_length);

    // 第二字段给出后续 12 字节 BPM 记录数量，循环内仍逐条校验实际长度。
    CHECK_BOUNDS(4);
    auto timing_point_amount = reader.read_value<int32_t>(data_pos);
    data_pos += 4;
    XINFO("读取到imd文件时间线数量: {}", timing_point_amount);

    // 每条时间记录由 int32 毫秒时间戳和 double BPM 组成。
    // IMD 只能表达 BPM 效果，因此读入后补齐统一 Timing 所需的拍长与参数。
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

        // 连续的相同 BPM 记录不改变节奏，去重可避免编辑器生成冗余时间点。
        // 首个有效时间点同时定义谱面的偏好 BPM。
        if ( beatMap.m_timings.empty() ||
             std::abs(read_timing.m_bpm - referenceTiming.m_bpm) > 0.0001 ) {
            referenceTiming = read_timing;
            if ( beatMap.m_timings.empty() ) {
                basemeta.preference_bpm = read_timing.m_bpm;
            }
            beatMap.m_timings.push_back(read_timing);
        }
    }

    // 历史样本在时间表后固定保留 0x0303；语义未知，只按布局跳过。
    CHECK_BOUNDS(2);
    data_pos += 2;

    // 表格行数是原格式声明值，物件读取仍以文件尾为准以兼容计数失真的样本。
    CHECK_BOUNDS(4);
    auto table_rows       = reader.read_value<int32_t>(data_pos);
    rmMapProps["tabRows"] = std::to_string(table_rows);
    data_pos += 4;
    XINFO("读取到表格行数: {}", table_rows);

    // 物件记录先进入各自拥有容器。折线暂存类型与索引，待容器增长结束后
    // 再建立 reference_wrapper，避免 vector 扩容让中途保存的地址失效。
    // all_polylines_indices 与已完成的 Polyline 一一对应；内部 pair 的类型决定
    // 应从哪个拥有容器取回节点，索引则是在插入当时记录的逻辑位置。
    // current_polyline_indices 只属于正在解析的一条折线，遇到新头时重新开始。
    Note*    temp_note_ptr{ nullptr };
    Polyline current_polyline;
    bool     is_building_polyline = false;
    int      obj_count{ 0 };

    std::vector<std::vector<std::pair<NoteType, size_t>>> all_polylines_indices;
    std::vector<std::pair<NoteType, size_t>> current_polyline_indices;

    // 每条物件固定占 11 字节：类型/折线标志、保留字节、时间戳、轨道和参数。
    // 高四位描述折线位置，低四位描述 Note、Flick 或 Hold 的基础类型。
    while ( data_pos < data_end ) {
        CHECK_BOUNDS(11);
        auto note_type_info    = reader.read_value<int8_t>(data_pos);
        auto note_complex_info = note_type_info & 0xf0;
        auto note_type         = note_type_info & 0x0f;
        // 第二字节为保留位，当前样本恒为零但仍必须消费以维持后续字段对齐。
        data_pos += 2;

        auto note_timestamp = reader.read_value<int32_t>(data_pos);
        data_pos += 4;

        auto note_track = reader.read_value<uint8_t>(data_pos);
        data_pos += 1;

        auto note_parameter = reader.read_value<int32_t>(data_pos);
        data_pos += 4;

        // 文件名中的轨道数和声明时长可能过旧，以实际物件补足模型边界。
        basemeta.track_count =
            std::max(basemeta.track_count, (int32_t)note_track + 1);
        basemeta.map_length =
            std::max((int64_t)basemeta.map_length, (int64_t)note_timestamp);

        // 同一个 parameter 字段按基础类型解释：Flick 为目标轨，Hold 为时长，
        // 普通 Note 则保留到 RM 专属元数据供再次导出。
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

        // 折线头开启一段连续记录。Polyline 自身不额外占用 IMD 物件行，
        // 它的时间与轨道来自首个子物件。
        // 若损坏文件在上一条折线未闭合时再次出现头标志，新头会替换临时状态，
        // 已拥有的基础物件仍保留为独立对象，不制造悬空引用。
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
            // 此处记录拥有容器中的稳定逻辑位置，而不是可能因扩容失效的地址。
            size_t idx = 0;
            if ( temp_note_ptr->m_type == NoteType::NOTE ) {
                idx = beatMap.m_noteData.notes.size() - 1;
            } else if ( temp_note_ptr->m_type == NoteType::FLICK ) {
                idx = beatMap.m_noteData.flicks.size() - 1;
            } else if ( temp_note_ptr->m_type == NoteType::HOLD ) {
                idx = beatMap.m_noteData.holds.size() - 1;
            }
            current_polyline_indices.push_back({ temp_note_ptr->m_type, idx });

            // 尾标志封闭当前折线；中间标志只延续当前索引序列。
            // 只有看见尾标志的序列才加入 Polyline 容器；文件尾处未闭合的序列
            // 不会形成结构不完整的折线，但其中已解析的基础物件仍可被恢复。
            if ( note_complex_info == 0xa0 ) {
                beatMap.m_noteData.polylines.push_back(current_polyline);
                all_polylines_indices.push_back(current_polyline_indices);
                is_building_polyline = false;
            }
        }
        obj_count++;
    }

#undef CHECK_BOUNDS

    XINFO("读取到物件总数: {}", obj_count);

    // 所有基础物件完成插入后，各容器容量不再变化，此时才安全解析索引并
    // 建立折线子物件引用。分类引用用于快速访问 Hold/Flick 子集，通用引用
    // 则保留折线原始节点顺序。
    // 同一个 Hold 会同时进入 m_subNotes 和 m_subHolds，同一个 Flick 会同时进入
    // m_subNotes 和 m_subFlicks；这些容器表达不同视图，不代表重复所有权。
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

    // RM/IMD 通过文件名前缀隐式引用单音频。统一模型要求显式采样事件，
    // 因此在玩家轨之后创建首条 BGM 轨，并用零时间、零偏移和原音量表达
    // 格式隐含的“从曲首播放一次”语义。
    if ( !basemeta.song_file_hint.empty() ) {
        AudioSampleEvent& sample = beatMap.m_audioSamples.emplace_back();
        sample.m_timestamp       = 0.0;
        sample.m_offsetMs        = 0;
        sample.m_track =
            static_cast<uint32_t>(std::max(0, basemeta.track_count));
        sample.m_audioResourceId = Config::pathToUtf8(basemeta.song_file_hint);
        sample.m_volume          = 1.0F;
        basemeta.bgm_track_count = std::max(1, basemeta.bgm_track_count);
    }

    // sync 在全部拥有容器和折线引用就绪后重建 m_allNotes 等派生索引。
    beatMap.sync();

    // 展示名不参与回写，只汇总已解析的格式、标题、轨道数与难度版本。
    basemeta.name =
        fmt::format("[rm] {} [{}k] {}",
                    (file_presuffix.empty() ? "Map" : file_presuffix),
                    basemeta.track_count,
                    basemeta.version);

    return beatMap;
}

}  // namespace MMM
