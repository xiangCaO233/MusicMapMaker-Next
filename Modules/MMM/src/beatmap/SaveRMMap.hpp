#pragma once

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace MMM
{

/// @file SaveRMMap.hpp
/// @brief 将统一谱面模型降级写入 RM/IMD 定宽二进制格式。
///
/// 写出顺序必须与加载器严格对应：
/// - 谱面时长 `int32`；
/// - BPM 数量 `int32`；
/// - 每个 BPM 的 `int32 timestamp` 与 `double bpm`；
/// - 固定标记 `int16 0x0303`；
/// - 物件行数 `int32`；
/// - 每行 11 字节物件数据。
///
/// IMD 的表达能力小于统一模型，导出前必须检查以下限制：
/// - 玩家物件及折线节点都不能绑定独立击打采样；
/// - 自动采样至多一个，并且必须等价于曲首播放主音频；
/// - 主音频必须位于谱面同目录且与输出文件标题前缀同名；
/// - 音频事件必须位于声明的 BGM 轨范围；
/// - 音频偏移必须为零，音量必须为一；
/// - 时间表只能写出 BPM 效果，其他效果不进入 IMD 节奏表。
///
/// 物件参数按类型映射：
/// - Note：优先恢复导入时保留的 RM `Parameter`；
/// - Flick：写入 `m_dtrack`；
/// - Hold：写入取整后尾时间与起时间之差；
/// - Polyline：容器不占记录，逐个编码其中的基础节点。
///
/// 折线必须作为不可拆分的记录块参与排序。若先把全部节点加入一个全局数组
/// 再按时间戳排序，其他同时间物件可能插入折线头尾之间，使加载器错误地把它
/// 识别成折线节点。因此这里只按每个条目块的首时间排序，随后再扁平化。
///
/// 导出器维持以下一致性约束：
/// - 可表达性检查在创建目标文件之前完成；
/// - 计数字段和实际写出的记录数量使用同一截断边界；
/// - 同时间条目使用稳定排序，保留模型中的既有顺序；
/// - 折线节点顺序来自 `m_subNotes`，不按节点时间二次排序；
/// - `m_allNotes` 中的折线子节点别名不会作为独立物件重复导出；
/// - 导入保留的 `tabRows` 仅用于诊断，不能覆盖实际计算值；
/// - 日志路径统一转换为 UTF-8，不依赖平台原生字符宽度。
///
/// 精确往返与规范化的边界如下：
/// - 原始 `mapLength` 存在时优先恢复原声明值；
/// - 普通 Note 的未知 parameter 会保留；
/// - 固定保留字节规范化为零；
/// - 物件按条目首时间稳定排序；
/// - 浮点时间戳会规范化为最接近的整数毫秒；
/// - 连续折线仍保持头、中、尾标志与节点顺序。
///
/// 维护此文件时应同步检查 `LoadRMMap.hpp` 与 `compareIMDChunks`：新增字段必须
/// 同时定义读取顺序、写出顺序及测试区块边界。由于格式没有版本号或字段标签，
/// 任意插入、遗漏或宽度变化都会使其后的所有记录错位。不要以结构体整体写出
/// 代替逐字段写出，编译器填充字节并不是文件格式的一部分。也不要依赖宿主类型
/// 的隐式宽度；磁盘字段必须继续使用 `int8_t`、`uint8_t`、`int16_t`、`int32_t`
/// 和 `double` 等明确类型。若未来支持大端平台，应在唯一的标量写入边界集中
/// 处理字节序，不能让各字段调用点分别决定转换策略。

/// @brief 解析 RM/IMD 二进制 int32 字段，不使用 C++ 异常。
/// @param text 必须完整表示一个十进制 int32，不接受尾随字符。
/// @return 成功时返回数值，空串、溢出或格式错误时返回空值。
inline std::optional<int32_t> parseRMInt32(std::string_view text)
{
    if ( text.empty() ) return std::nullopt;

    int32_t     value = 0;
    const char* begin = text.data();
    const char* end   = begin + text.size();
    // from_chars 不依赖区域设置，也不会抛出异常，适合解析兼容元数据。
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if ( ec != std::errc{} || ptr != end ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 将谱面保存为 RM/IMD 二进制格式。
/// @param beatMap 已同步的统一谱面模型。
/// @param path 目标 IMD 文件路径，文件名应满足 Title_nk_Version.imd 约定。
/// @return 全部兼容性检查和文件写出成功时返回 true。
/// @details 导出器先拒绝 IMD 无法无损表达的采样绑定，再依次写入时长、BPM
/// 表、固定标记和扁平物件表。折线会保持为连续记录块，避免全局排序打散节点。
/// @note IMD 不携带完整资源引用，单音频必须能由输出文件名前缀唯一推导。
inline bool saveRMMap(const BeatMap& beatMap, std::filesystem::path path)
{
    /// @brief RM/IMD 隐式音频零点允许的最大时间误差，单位为毫秒。
    static constexpr double IMPLICIT_AUDIO_ZERO_TOLERANCE_MS = 1.0;

    /// @brief 判断任意玩家物件是否绑定了 RM/IMD 无法表达的采样。
    /// @details 同时检查独立物件、Polyline 自身和折线子节点；只检查顶层容器
    /// 会漏掉由引用持有、但未作为独立条目导出的子物件。
    const auto hasUnsupportedNoteSampleBinding = [&beatMap]() {
        // 类型容器中的检查覆盖普通 Note、Hold、Flick 以及 Polyline 容器本身。
        // Polyline 子节点是引用视图，还需单独遍历，不能假定它们总能在某个
        // 顶层类型容器的同一访问路径中被完整观察到。
        const auto containsBinding = [](const auto& notes) {
            return std::any_of(
                notes.begin(), notes.end(), [](const auto& note) {
                    return note.getSampleBinding().has_value();
                });
        };
        // 快速覆盖各拥有容器，再补查折线引用中的节点。
        if ( containsBinding(beatMap.m_noteData.notes) ||
             containsBinding(beatMap.m_noteData.holds) ||
             containsBinding(beatMap.m_noteData.flicks) ||
             containsBinding(beatMap.m_noteData.polylines) ) {
            return true;
        }
        return std::any_of(
            beatMap.m_noteData.polylines.begin(),
            beatMap.m_noteData.polylines.end(),
            [](const Polyline& polyline) {
                return std::any_of(
                    polyline.m_subNotes.begin(),
                    polyline.m_subNotes.end(),
                    [](const auto& noteRef) {
                        return noteRef.get().getSampleBinding().has_value();
                    });
            });
    };
    // 静默丢弃绑定会改变击打音效语义，因此在打开输出文件前明确失败。
    if ( hasUnsupportedNoteSampleBinding() ) {
        XERROR("RM/IMD 导出失败：格式无法表达玩家物件采样绑定");
        return false;
    }

    // IMD 的音频是“同名前缀文件从零点播放”的隐含约定，只能映射一个事件。
    const AudioSampleEvent* legacyAudioSample = nullptr;
    if ( beatMap.m_audioSamples.size() > 1 ) {
        XERROR(
            "RM/IMD 导出失败：格式只能表达一个隐式音频，当前有 {} "
            "个自动采样对象",
            beatMap.m_audioSamples.size());
        return false;
    }
    if ( !beatMap.m_audioSamples.empty() ) {
        // 统一模型中的显式采样必须严格等价于 IMD 隐式语义，否则拒绝降级。
        // 时间戳使用有限容差，是为了接纳浮点换算产生的亚毫秒误差；offset、
        // track 和 volume 没有相同的格式模糊性，因此按其精确契约验证。
        legacyAudioSample            = &beatMap.m_audioSamples.front();
        const uint32_t firstBgmTrack = static_cast<uint32_t>(
            std::max(0, beatMap.m_baseMapMetadata.track_count));
        // 使用 64 位计算区间上界，避免异常大的轨道计数发生无符号回绕。
        const uint64_t declaredBgmTrackEnd =
            static_cast<uint64_t>(firstBgmTrack) +
            static_cast<uint64_t>(
                std::max(0, beatMap.m_baseMapMetadata.bgm_track_count));
        if ( legacyAudioSample->m_audioResourceId.empty() ||
             !std::isfinite(legacyAudioSample->m_timestamp) ||
             std::abs(legacyAudioSample->m_timestamp) >
                 IMPLICIT_AUDIO_ZERO_TOLERANCE_MS ||
             legacyAudioSample->m_offsetMs != 0 ||
             legacyAudioSample->m_track < firstBgmTrack ||
             static_cast<uint64_t>(legacyAudioSample->m_track) >=
                 declaredBgmTrackEnd ||
             !std::isfinite(legacyAudioSample->m_volume) ||
             std::abs(legacyAudioSample->m_volume - 1.0F) > 1e-6F ) {
            XERROR(
                "RM/IMD 导出失败：自动采样必须是 |timestamp|<=1 ms、"
                "offset=0、"
                "track 位于已声明 BGM 轨范围 [{}, {})、volume=1 "
                "且音频引用非空；当前为 "
                "timestamp={}、offset={}、track={}、volume={}、ref='{}'",
                firstBgmTrack,
                declaredBgmTrackEnd,
                legacyAudioSample->m_timestamp,
                legacyAudioSample->m_offsetMs,
                legacyAudioSample->m_track,
                legacyAudioSample->m_volume,
                legacyAudioSample->m_audioResourceId);
            return false;
        }

        // 输出文件名的标题段必须与资源 stem 一致，且资源不能位于子目录。
        // 这是加载器无需额外清单即可重新找到相同音频的必要条件。
        const std::string outputFilename = Config::pathToUtf8(path.filename());
        const size_t      firstSeparator = outputFilename.find('_');
        const std::string audioPrefix =
            firstSeparator == std::string::npos
                ? std::string{}
                : outputFilename.substr(0, firstSeparator);
        const std::filesystem::path audioReference =
            Config::utf8ToPath(legacyAudioSample->m_audioResourceId);
        // 与加载器共享相同的可发现扩展名集合，防止写出后无法重新定位。
        static constexpr std::array<std::string_view, 7> AUDIO_EXTENSIONS{
            ".mp3", ".wav", ".ogg", ".flac", ".opus", ".aac", ".m4a"
        };
        const std::string audioExtension =
            Config::pathToUtf8(audioReference.extension());
        const bool supportedExtension =
            std::find(AUDIO_EXTENSIONS.begin(),
                      AUDIO_EXTENSIONS.end(),
                      audioExtension) != AUDIO_EXTENSIONS.end();
        if ( audioPrefix.empty() || audioReference.has_parent_path() ||
             Config::pathToUtf8(audioReference.stem()) != audioPrefix ||
             !supportedExtension ) {
            XERROR(
                "RM/IMD 导出失败：音频引用 '{}' 无法由输出文件名 '{}' "
                "的同名前缀规则表达",
                legacyAudioSample->m_audioResourceId,
                outputFilename);
            return false;
        }
        // legacyAudioSample 此后无需写入文件体；通过验证即意味着文件名前缀
        // 已能完整承载它的资源身份，加载器会据此重新构造显式采样事件。
    }

    // 所有可表达性检查结束后才创建文件，避免失败导出留下半成品。
    std::ofstream ofs(path, std::ios::binary);
    if ( !ofs ) {
        XWARN("无法打开文件 [{}] 进行 RM/IMD 写出", Config::pathToUtf8(path));
        return false;
    }

    /// @brief 按主机小端字节序写入一个定宽字段。
    /// @note 当前受支持平台与 IMD 格式均采用小端；字段顺序由调用处明确控制。
    auto write_value = [&ofs](auto value) {
        // 逐字段写入保持格式布局直观；ofstream 状态在函数结束前统一体现。
        ofs.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };

    /// @brief 查询导入时保留的 RM 专属谱面属性。
    /// @return 属性存在时返回稳定引用，否则返回空指针。
    auto get_rm_map_property =
        [&beatMap](const std::string& key) -> const std::string* {
        auto propsIt =
            beatMap.m_metadata.map_properties.find(MapMetadataType::RM);
        if ( propsIt == beatMap.m_metadata.map_properties.end() ) {
            return nullptr;
        }

        auto valueIt = propsIt->second.find(key);
        if ( valueIt == propsIt->second.end() ) {
            return nullptr;
        }

        return &valueIt->second;
    };

    /// @brief 读取并严格解析一个 RM 专属 int32 属性。
    /// @return 属性缺失或格式非法时返回空值。
    auto get_rm_map_int32 =
        [&get_rm_map_property](
            const std::string& key) -> std::optional<int32_t> {
        const auto* value = get_rm_map_property(key);
        if ( value == nullptr ) {
            return std::nullopt;
        }
        return parseRMInt32(*value);
    };

    // 首字段为 int32 谱面时长。先扫描实际物件结束点，避免陈旧元数据截短谱面；
    // Hold 使用尾时间，Polyline 还需逐节点检查其中可能存在的 Hold。
    double calculated_map_length = beatMap.m_baseMapMetadata.map_length;
    for ( const auto& note_ref : beatMap.m_allNotes ) {
        const auto& note = note_ref.get();
        double      end  = note.m_timestamp;
        if ( note.m_type == NoteType::HOLD ) {
            // 独立长条的结束位置是起点加持续时间。
            end += static_cast<const Hold&>(note).m_duration;
        } else if ( note.m_type == NoteType::POLYLINE ) {
            // 折线容器的时间戳不能代表最后节点，必须检查完整节点序列。
            const auto& poly = static_cast<const Polyline&>(note);
            for ( const auto& sn_ref : poly.m_subNotes ) {
                const auto& sn    = sn_ref.get();
                double      snEnd = sn.m_timestamp;
                if ( sn.m_type == NoteType::HOLD ) {
                    snEnd += static_cast<const Hold&>(sn).m_duration;
                }
                if ( snEnd > calculated_map_length )
                    calculated_map_length = snEnd;
            }
        }
        if ( end > calculated_map_length ) calculated_map_length = end;
    }
    // Timing 点也属于有效内容，声明时长至少应覆盖最后一次节奏变化。
    for ( const auto& timing : beatMap.m_timings ) {
        if ( timing.m_timestamp > calculated_map_length )
            calculated_map_length = timing.m_timestamp;
    }

    // 从 IMD 导入时保存的原始声明值优先，以便未编辑文件实现字节级往返；
    // 新建谱面或缺失扩展属性时才使用根据当前模型计算的时长。
    // 该兼容值可能小于重新计算值，这是原格式往返的有意选择；编辑流程负责在
    // 语义发生变化时同步或移除相应扩展属性。
    int32_t output_map_length =
        static_cast<int32_t>(std::round(calculated_map_length));
    if ( auto value = get_rm_map_int32("mapLength") ) {
        output_map_length = *value;
    }
    write_value(output_map_length);

    // IMD 只支持 BPM 时间点，Scroll 等统一模型效果不能写入该二进制表。
    // reference_wrapper 避免复制 Timing，同时保持输出次序与模型次序一致。
    std::vector<std::reference_wrapper<const Timing>> bpm_timings;
    bpm_timings.reserve(beatMap.m_timings.size());
    for ( const auto& timing : beatMap.m_timings ) {
        if ( timing.m_timingEffect == TimingEffect::BPM ) {
            bpm_timings.push_back(std::cref(timing));
        }
    }

    // 第二字段是 BPM 表条数；计数受 int32 表达范围约束。
    int32_t timing_count = static_cast<int32_t>(std::min<size_t>(
        bpm_timings.size(),
        static_cast<size_t>(std::numeric_limits<int32_t>::max())));
    write_value(timing_count);

    // 每条记录是 int32 毫秒时间戳和 double BPM；时间戳按最近整数取整。
    for ( int32_t i = 0; i < timing_count; ++i ) {
        const auto& timing = bpm_timings[static_cast<size_t>(i)].get();
        write_value(static_cast<int32_t>(std::round(timing.m_timestamp)));
        write_value(static_cast<double>(timing.m_bpm));  // BPM
    }

    // 时间表后的 0x0303 是历史格式固定标记，虽语义未知仍须原样生成。
    int16_t magic_unknown = 0x0303;
    write_value(magic_unknown);

    // 物件表把独立物件和折线子物件都表示为一行，Polyline 本身不占行。
    int32_t table_rows = 0;

    // 先形成与磁盘布局一一对应的中间记录。高四位编码折线位置，低四位编码
    // 基础物件类型；其余三个字段直接对应磁盘中的时间戳、轨道和参数。
    /// @brief 单条 IMD 物件记录的内存表示。
    struct RMNoteRecord {
        /// @brief 高四位为折线标志，低四位为基础物件类型。
        int8_t note_type_info;
        /// @brief 取整后的毫秒时间戳。
        int32_t note_timestamp;
        /// @brief 单字节玩家轨道编号。
        uint8_t note_track;
        /// @brief 普通物件保留值、Flick 目标轨或 Hold 持续时间。
        int32_t note_parameter;
    };
    std::vector<RMNoteRecord> rm_records;

    /// @brief 把统一 Note 转换为一条 IMD 定宽记录。
    /// @param note 要编码的基础物件；Polyline 容器不会传入此函数。
    /// @param complex_info 折线头、中、尾或独立物件标志。
    /// @return 完整的中间记录。
    auto make_record = [](const Note& note,
                          uint8_t     complex_info) -> RMNoteRecord {
        RMNoteRecord rec;
        int8_t       base_type = 0;
        int32_t      param     = 0;
        int32_t      rounded_start =
            static_cast<int32_t>(std::round(note.m_timestamp));
        // IMD 的时间字段没有小数表示；统一在记录构造处取整，避免不同类型使用
        // 不一致的转换规则。

        if ( note.m_type == NoteType::HOLD ) {
            // 起点和终点分别取整后相减，可保证重载后的整数尾时间一致。
            base_type        = 2;
            double  duration = static_cast<const Hold&>(note).m_duration;
            int32_t rounded_end =
                static_cast<int32_t>(std::round(note.m_timestamp + duration));
            // 分别取整端点而不是直接取整 duration，确保写出的尾时间最接近模型。
            param = rounded_end - rounded_start;
        } else if ( note.m_type == NoteType::FLICK ) {
            // Flick 参数在 IMD 中复用为目标轨道偏移。
            base_type = 1;
            param =
                static_cast<int32_t>(static_cast<const Flick&>(note).m_dtrack);
        } else {
            // 普通 Note 的未知参数不影响统一语义，但保留它可实现精确往返。
            if ( note.m_metadata.note_properties.contains(
                     NoteMetadataType::RM) ) {
                const auto& rm_props =
                    note.m_metadata.note_properties.at(NoteMetadataType::RM);
                if ( rm_props.contains("Parameter") ) {
                    // 非法兼容字段回退到零，禁止异常文本污染定宽二进制输出。
                    param = parseRMInt32(rm_props.at("Parameter")).value_or(0);
                }
            }
            base_type = 0;
        }

        rec.note_type_info = static_cast<int8_t>(complex_info | base_type);
        rec.note_timestamp = rounded_start;
        rec.note_track     = static_cast<uint8_t>(note.m_track);
        rec.note_parameter = param;
        return rec;
    };

    // sync 会把折线子物件也加入 m_allNotes。地址集合只用于识别这些别名，
    // 不参与排序或所有权管理；节点仍由 NoteData 的类型容器拥有。
    // 若不排除，子物件会先作为独立行写出，再在折线块中重复写出一次。
    std::unordered_set<const Note*> subnote_ptrs;
    // 地址只在当前函数内使用；导出过程中不修改拥有容器，因此其生命周期稳定。
    for ( const auto& poly : beatMap.m_noteData.polylines ) {
        for ( const auto& subref : poly.m_subNotes ) {
            subnote_ptrs.insert(&subref.get());
        }
    }

    /// @brief 可参与全局排序的原子条目块。
    /// @details 独立物件只有一条记录，折线则把全部节点作为不可拆分的连续块。
    struct RMEntry {
        /// @brief 用于块间排序的首个物件时间戳。
        double first_timestamp;
        /// @brief 保持原始节点顺序的连续磁盘记录。
        std::vector<RMNoteRecord> records;
    };
    std::vector<RMEntry> entries;

    for ( const auto& note_ref : beatMap.m_allNotes ) {
        // m_allNotes 提供跨类型的统一时间视图，条目构造不需要重复遍历每个容器。
        const Note& n = note_ref.get();
        if ( subnote_ptrs.find(&n) != subnote_ptrs.end() ) {
            // 子物件随所属 Polyline 块统一处理，不能作为独立条目再次输出。
            continue;
        }
        if ( n.m_type == NoteType::POLYLINE ) {
            // 空折线没有可表示的 IMD 行，非空折线按节点顺序编码连续块。
            const Polyline& poly = static_cast<const Polyline&>(n);
            if ( poly.m_subNotes.empty() ) continue;

            RMEntry entry;
            entry.first_timestamp = poly.m_subNotes.front().get().m_timestamp;
            // 首节点时间定义整个块的排序位置；后续节点即使时间异常，也不能被
            // 拆出该块，否则头尾标志在文件流中将失去配对关系。
            for ( size_t i = 0; i < poly.m_subNotes.size(); ++i ) {
                uint8_t complex = 0;
                // 头和尾标志界定折线；中间节点使用延续标志。
                if ( i == 0 )
                    complex = 0x60;
                else if ( i == poly.m_subNotes.size() - 1 )
                    complex = 0xa0;
                else
                    complex = 0x20;

                entry.records.push_back(
                    make_record(poly.m_subNotes[i].get(), complex));
            }
            entries.push_back(std::move(entry));
        } else {
            // 独立物件使用零复杂标志，并作为单记录条目参与排序。
            RMEntry entry;
            entry.first_timestamp = n.m_timestamp;
            entry.records.push_back(make_record(n, 0x00));
            entries.push_back(std::move(entry));
        }
    }

    // 只在条目块之间按首时间戳排序，不能对最终记录逐行排序，否则会打散折线。
    // stable_sort 让同一时间戳的条目保持模型中的既有顺序。
    std::stable_sort(
        entries.begin(), entries.end(), [](const RMEntry& a, const RMEntry& b) {
            return a.first_timestamp < b.first_timestamp;
        });

    // 排序完成后再展平，折线子物件因此保持连续且内部顺序不变。
    // 此处有意复制小型定宽记录，最终数组随后用于统一计算行数与顺序写出。
    rm_records.clear();
    for ( const auto& entry : entries ) {
        for ( const auto& rec : entry.records ) {
            rm_records.push_back(rec);
        }
    }

    // 文件头只能写 int32 行数；超限时记录与计数必须按同一边界截断。
    const size_t max_rows =
        static_cast<size_t>(std::numeric_limits<int32_t>::max());
    const size_t writable_record_count = std::min(rm_records.size(), max_rows);
    table_rows = static_cast<int32_t>(writable_record_count);

    // tabRows 是输入文件的兼容元数据，当前模型才是实际输出行数的权威来源。
    // 声明失效时只记录诊断，不写出不一致计数导致重载越界。
    const std::string* tab_rows_text = get_rm_map_property("tabRows");
    if ( tab_rows_text != nullptr ) {
        const auto declared_table_rows = parseRMInt32(*tab_rows_text);
        if ( !declared_table_rows.has_value() ) {
            XWARN(
                "RM/IMD 导出: extra.tabRows='{}' 不是合法 "
                "int32，已修正为实际物件行数 {}",
                *tab_rows_text,
                table_rows);
        } else if ( *declared_table_rows != table_rows ) {
            XWARN(
                "RM/IMD 导出: extra.tabRows={} 与实际导出物件行数 {} "
                "不一致，已修正为 {}",
                *declared_table_rows,
                table_rows,
                table_rows);
        }
    }

    if ( rm_records.size() > writable_record_count ) {
        // 发生截断时仍写出自洽的 tableRows，加载器不会尝试读取不存在的行。
        XWARN("RM/IMD 导出: 物件行数 {} 超过 int32 上限 {}，已截断导出",
              rm_records.size(),
              table_rows);
    }
    write_value(table_rows);

    // 记录字段严格按照加载器消费顺序写出，保留字节固定为零。
    // note_type_info 与 zero8 分开写，明确保证每条记录头部恰好两个字节。
    for ( size_t i = 0; i < writable_record_count; ++i ) {
        const auto& rec = rm_records[i];
        write_value(rec.note_type_info);
        int8_t zero8 = 0;
        write_value(zero8);  // 固定没用的 00
        write_value(rec.note_timestamp);
        write_value(rec.note_track);
        write_value(rec.note_parameter);
    }

    // 日志边界统一转换为 UTF-8，避免平台原生 path 字符类型泄漏到 fmt。
    auto pathToStr = [](const std::filesystem::path& p) {
        auto u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.c_str()),
                           u8.size());
    };
    XINFO("Successfully saved RM map to {}", pathToStr(path));
    return true;
}

}  // namespace MMM
