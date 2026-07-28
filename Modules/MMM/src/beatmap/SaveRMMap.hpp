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

/// @brief 解析 RM/IMD 二进制 int32 字段，不使用 C++ 异常。
inline std::optional<int32_t> parseRMInt32(std::string_view text)
{
    if ( text.empty() ) return std::nullopt;

    int32_t     value    = 0;
    const char* begin    = text.data();
    const char* end      = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if ( ec != std::errc{} || ptr != end ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 将谱面保存为 RM/IMD 二进制格式。
inline bool saveRMMap(const BeatMap& beatMap, std::filesystem::path path)
{
    /// @brief 判断任意玩家物件是否绑定了 RM/IMD 导出器无法表达的采样。
    const auto hasUnsupportedNoteSampleBinding = [&beatMap]() {
        const auto containsBinding = [](const auto& notes) {
            return std::any_of(
                notes.begin(), notes.end(), [](const auto& note) {
                    return note.getSampleBinding().has_value();
                });
        };
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
    if ( hasUnsupportedNoteSampleBinding() ) {
        XERROR("RM/IMD 导出失败：格式无法表达玩家物件采样绑定");
        return false;
    }

    const int representableBgmTrackCount =
        beatMap.m_audioSamples.empty() ? 0 : 1;
    if ( beatMap.m_baseMapMetadata.bgm_track_count !=
         representableBgmTrackCount ) {
        XERROR(
            "RM/IMD 导出失败：格式只能由单音频隐式表达 {} 条 BGM "
            "轨，当前谱面显式保存了 {} 条",
            representableBgmTrackCount,
            beatMap.m_baseMapMetadata.bgm_track_count);
        return false;
    }

    const AudioSampleEvent* legacyAudioSample = nullptr;
    if ( beatMap.m_audioSamples.size() > 1 ) {
        XERROR(
            "RM/IMD 导出失败：格式只能表达一个隐式音频，当前有 {} "
            "个自动采样对象",
            beatMap.m_audioSamples.size());
        return false;
    }
    if ( !beatMap.m_audioSamples.empty() ) {
        legacyAudioSample            = &beatMap.m_audioSamples.front();
        const uint32_t firstBgmTrack = static_cast<uint32_t>(
            std::max(0, beatMap.m_baseMapMetadata.track_count));
        if ( legacyAudioSample->m_audioResourceId.empty() ||
             !std::isfinite(legacyAudioSample->m_timestamp) ||
             std::abs(legacyAudioSample->m_timestamp) > 1e-6 ||
             legacyAudioSample->m_offsetMs != 0 ||
             legacyAudioSample->m_track != firstBgmTrack ||
             !std::isfinite(legacyAudioSample->m_volume) ||
             std::abs(legacyAudioSample->m_volume - 1.0F) > 1e-6F ) {
            XERROR(
                "RM/IMD 导出失败：自动采样必须是 timestamp=0、offset=0、"
                "track={}、volume=1 且音频引用非空；当前为 "
                "timestamp={}、offset={}、track={}、volume={}、ref='{}'",
                firstBgmTrack,
                legacyAudioSample->m_timestamp,
                legacyAudioSample->m_offsetMs,
                legacyAudioSample->m_track,
                legacyAudioSample->m_volume,
                legacyAudioSample->m_audioResourceId);
            return false;
        }

        const std::string outputFilename = Config::pathToUtf8(path.filename());
        const size_t      firstSeparator = outputFilename.find('_');
        const std::string audioPrefix =
            firstSeparator == std::string::npos
                ? std::string{}
                : outputFilename.substr(0, firstSeparator);
        const std::filesystem::path audioReference =
            Config::utf8ToPath(legacyAudioSample->m_audioResourceId);
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
    }

    std::ofstream ofs(path, std::ios::binary);
    if ( !ofs ) {
        XWARN("无法打开文件 [{}] 进行 RM/IMD 写出", Config::pathToUtf8(path));
        return false;
    }

    auto write_value = [&ofs](auto value) {
        ofs.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };

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

    auto get_rm_map_int32 =
        [&get_rm_map_property](
            const std::string& key) -> std::optional<int32_t> {
        const auto* value = get_rm_map_property(key);
        if ( value == nullptr ) {
            return std::nullopt;
        }
        return parseRMInt32(*value);
    };

    // 0~4字节:int32 谱面时长
    // 鲁棒性改进：在导出时重新扫描所有物件，计算最晚结束时间，防止元数据同步滞后导致的时长错误
    double calculated_map_length = beatMap.m_baseMapMetadata.map_length;
    for ( const auto& note_ref : beatMap.m_allNotes ) {
        const auto& note = note_ref.get();
        double      end  = note.m_timestamp;
        if ( note.m_type == NoteType::HOLD ) {
            end += static_cast<const Hold&>(note).m_duration;
        } else if ( note.m_type == NoteType::POLYLINE ) {
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
    // 同时也检查 Timing 点，确保谱面时长至少覆盖到最后一个 BPM/Scroll 变化
    for ( const auto& timing : beatMap.m_timings ) {
        if ( timing.m_timestamp > calculated_map_length )
            calculated_map_length = timing.m_timestamp;
    }

    int32_t output_map_length =
        static_cast<int32_t>(std::round(calculated_map_length));
    if ( auto value = get_rm_map_int32("mapLength") ) {
        output_map_length = *value;
    }
    write_value(output_map_length);

    std::vector<std::reference_wrapper<const Timing>> bpm_timings;
    bpm_timings.reserve(beatMap.m_timings.size());
    for ( const auto& timing : beatMap.m_timings ) {
        if ( timing.m_timingEffect == TimingEffect::BPM ) {
            bpm_timings.push_back(std::cref(timing));
        }
    }

    // 5~8字节:int32 图时间点数，仅保存 IMD 支持的 BPM timing
    int32_t timing_count = static_cast<int32_t>(std::min<size_t>(
        bpm_timings.size(),
        static_cast<size_t>(std::numeric_limits<int32_t>::max())));
    write_value(timing_count);

    // 每12字节一组: 4字节int32 时间戳 + 8字节double bpm
    for ( int32_t i = 0; i < timing_count; ++i ) {
        const auto& timing = bpm_timings[static_cast<size_t>(i)].get();
        write_value(static_cast<int32_t>(std::round(timing.m_timestamp)));
        write_value(static_cast<double>(timing.m_bpm));  // BPM
    }

    // 一个03 03未知意义的int16
    int16_t magic_unknown = 0x0303;
    write_value(magic_unknown);

    // 统计总的有效物件数 (RM 的表中，单个音符、折线子物件都是一行)
    int32_t table_rows = 0;

    // 我们需要把所有的非子物件和折线内的子物件扁平化，并且标记折线的头部、中间、尾部
    // 为保证时间戳有序，先扁平化到一个列表中，包含所有需要的属性
    struct RMNoteRecord {
        int8_t  note_type_info;
        int32_t note_timestamp;
        uint8_t note_track;
        int32_t note_parameter;
    };
    std::vector<RMNoteRecord> rm_records;

    // 为了避免重复导出独立的 sub_note，我们遍历所有 Note 并排除属于 Polyline
    // 的独立 note。 但是这里最安全的方法是直接从 NoteData 中重新遍历。

    auto make_record = [](const Note& note,
                          uint8_t     complex_info) -> RMNoteRecord {
        RMNoteRecord rec;
        int8_t       base_type = 0;
        int32_t      param     = 0;
        int32_t      rounded_start =
            static_cast<int32_t>(std::round(note.m_timestamp));

        if ( note.m_type == NoteType::HOLD ) {
            base_type        = 2;
            double  duration = static_cast<const Hold&>(note).m_duration;
            int32_t rounded_end =
                static_cast<int32_t>(std::round(note.m_timestamp + duration));
            param = rounded_end - rounded_start;
        } else if ( note.m_type == NoteType::FLICK ) {
            base_type = 1;
            param =
                static_cast<int32_t>(static_cast<const Flick&>(note).m_dtrack);
        } else {
            if ( note.m_metadata.note_properties.contains(
                     NoteMetadataType::RM) ) {
                const auto& rm_props =
                    note.m_metadata.note_properties.at(NoteMetadataType::RM);
                if ( rm_props.contains("Parameter") ) {
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

    // 【查重过滤器】：由于 syncBeatmap 会把子物件也放入 m_allNotes 中，
    // 我们需要用一个哈希集合 (unordered_set) 记录所有作为子物件的指针地址。
    // 这不是为了给子物件排序（子物件写出时严格遵循 poly.m_subNotes
    // 的原始数组顺序）， 而是为了在遍历 m_allNotes 时，能以 O(1)
    // 的速度判断并跳过这些子物件，避免重复导出。
    std::unordered_set<const Note*> subnote_ptrs;
    for ( const auto& poly : beatMap.m_noteData.polylines ) {
        for ( const auto& subref : poly.m_subNotes ) {
            subnote_ptrs.insert(&subref.get());
        }
    }

    /// @brief 排序条目：独立物件为单条记录，折线为连续的子物件块
    struct RMEntry {
        double                    first_timestamp;
        std::vector<RMNoteRecord> records;
    };
    std::vector<RMEntry> entries;

    for ( const auto& note_ref : beatMap.m_allNotes ) {
        const Note& n = note_ref.get();
        if ( subnote_ptrs.find(&n) != subnote_ptrs.end() ) {
            continue;  // 命中过滤器，说明它是子物件，跳过（它会在下面的
                       // Polyline 循环中被按顺序处理）
        }
        if ( n.m_type == NoteType::POLYLINE ) {
            // 是折线，将其子物件扁平化为连续块
            const Polyline& poly = static_cast<const Polyline&>(n);
            if ( poly.m_subNotes.empty() ) continue;

            RMEntry entry;
            entry.first_timestamp = poly.m_subNotes.front().get().m_timestamp;
            for ( size_t i = 0; i < poly.m_subNotes.size(); ++i ) {
                uint8_t complex = 0;
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
            // 独立物件
            RMEntry entry;
            entry.first_timestamp = n.m_timestamp;
            entry.records.push_back(make_record(n, 0x00));
            entries.push_back(std::move(entry));
        }
    }

    // 按首个时间戳排序条目块
    // stable_sort 确保相同时间戳的条目保持插入顺序
    std::stable_sort(
        entries.begin(), entries.end(), [](const RMEntry& a, const RMEntry& b) {
            return a.first_timestamp < b.first_timestamp;
        });

    // 展平为最终记录列表（折线子物件保持连续且顺序不变）
    rm_records.clear();
    for ( const auto& entry : entries ) {
        for ( const auto& rec : entry.records ) {
            rm_records.push_back(rec);
        }
    }

    const size_t max_rows =
        static_cast<size_t>(std::numeric_limits<int32_t>::max());
    const size_t writable_record_count = std::min(rm_records.size(), max_rows);
    table_rows = static_cast<int32_t>(writable_record_count);

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
        XWARN("RM/IMD 导出: 物件行数 {} 超过 int32 上限 {}，已截断导出",
              rm_records.size(),
              table_rows);
    }
    write_value(table_rows);

    for ( size_t i = 0; i < writable_record_count; ++i ) {
        const auto& rec = rm_records[i];
        write_value(rec.note_type_info);
        int8_t zero8 = 0;
        write_value(zero8);  // 固定没用的 00
        write_value(rec.note_timestamp);
        write_value(rec.note_track);
        write_value(rec.note_parameter);
    }

    auto pathToStr = [](const std::filesystem::path& p) {
        auto u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.c_str()),
                           u8.size());
    };
    XINFO("Successfully saved RM map to {}", pathToStr(path));
    return true;
}

}  // namespace MMM
