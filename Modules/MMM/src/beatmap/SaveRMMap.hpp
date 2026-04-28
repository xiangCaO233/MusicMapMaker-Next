#pragma once

#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <vector>

namespace MMM
{

inline bool saveRMMap(const BeatMap& beatMap, std::filesystem::path path)
{
    std::ofstream ofs(path, std::ios::binary);
    if ( !ofs ) {
        XWARN("无法打开文件 [{}] 进行 RM/IMD 写出", path.string());
        return false;
    }

    auto write_value = [&ofs](auto value) {
        ofs.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };

    // 0~4字节:int32 谱面时长
    write_value(static_cast<int32_t>(beatMap.m_baseMapMetadata.map_length));

    // 5~8字节:int32 图时间点数
    int32_t timing_count = static_cast<int32_t>(beatMap.m_timings.size());
    write_value(timing_count);

    // 每12字节一组: 4字节int32 时间戳 + 8字节double bpm
    for ( const auto& timing : beatMap.m_timings ) {
        write_value(static_cast<int32_t>(timing.m_timestamp));
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
        if ( note.m_type == NoteType::HOLD ) {
            base_type = 2;
            param =
                static_cast<int32_t>(static_cast<const Hold&>(note).m_duration);
        } else if ( note.m_type == NoteType::FLICK ) {
            base_type = 1;
            param =
                static_cast<int32_t>(static_cast<const Flick&>(note).m_dtrack);
        } else {
            if ( note.m_metadata.note_properties.contains(NoteMetadataType::RM) ) {
                const auto& rm_props = note.m_metadata.note_properties.at(NoteMetadataType::RM);
                if ( rm_props.contains("Parameter") ) {
                    param = std::stoi(rm_props.at("Parameter"));
                }
            }
            base_type = 0;
        }

        rec.note_type_info = static_cast<int8_t>(complex_info | base_type);
        rec.note_timestamp = static_cast<int32_t>(note.m_timestamp);
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

    table_rows = static_cast<int32_t>(rm_records.size());
    write_value(table_rows);

    for ( const auto& rec : rm_records ) {
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
