#pragma once

#include "log/colorful-log.h"
#include "mmm/SafeParse.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <numeric>
#include <set>
#include <vector>

namespace MMM
{

using json = nlohmann::json;

inline bool saveMalodyMap(const BeatMap& beatMap, std::filesystem::path path)
{
    json fileData;

    // 轨道数和默认宽度
    int trackCount = static_cast<int>(beatMap.m_baseMapMetadata.track_count);
    if ( trackCount <= 0 ) trackCount = 4;

    static const std::map<int, int> baseWMap = { { 4, 60 },
                                                 { 5, 50 },
                                                 { 6, 40 } };
    int defaultW = baseWMap.contains(trackCount)
                       ? baseWMap.at(trackCount)
                       : static_cast<int>(std::round(256.0 / trackCount));

    /// @brief 将轨道索引转换为 mode 7 的 x 坐标（画布宽度 256）
    auto columnToX = [&](int column) {
        return static_cast<int>(
            std::round((2 * column + 1) * 128.0 / trackCount));
    };

    // Meta
    auto& meta      = fileData["meta"];
    meta["creator"] = beatMap.m_baseMapMetadata.author;
    meta["version"] = beatMap.m_baseMapMetadata.version;
    meta["background"] =
        beatMap.m_baseMapMetadata.main_cover_path.filename().string();
    meta["id"]   = 0;
    meta["mode"] = 7;  // 默认使用坐标模式，与 Malody 编辑器一致

    auto& song        = meta["song"];
    song["title"]     = beatMap.m_baseMapMetadata.title;
    song["titleorg"]  = beatMap.m_baseMapMetadata.title_unicode;
    song["artist"]    = beatMap.m_baseMapMetadata.artist;
    song["artistorg"] = beatMap.m_baseMapMetadata.artist_unicode;
    song["file"] =
        beatMap.m_baseMapMetadata.main_audio_path.filename().string();
    song["bpm"] = beatMap.m_baseMapMetadata.preference_bpm;

    auto& mode_ext = meta["mode_ext"] = json::object();

    if ( auto it =
             beatMap.m_metadata.map_properties.find(MapMetadataType::MALODY);
         it != beatMap.m_metadata.map_properties.end() ) {
        for ( const auto& [key, val] : it->second ) {
            if ( key == "mode_ext" ) {
                try {
                    meta["mode_ext"] = json::parse(val);
                } catch ( ... ) {
                    meta["mode_ext"] = val;
                }
            } else if ( key == "id" || key == "preview" || key == "mode" ) {
                try {
                    meta[key] = std::stoll(val);
                } catch ( ... ) {
                    meta[key] = val;
                }
            } else {
                try {
                    meta[key] = json::parse(val);
                } catch ( ... ) {
                    meta[key] = val;
                }
            }
        }
    }

    int mode = meta.value("mode", 7);

    auto timeToBeat = [&](double time) {
        double currentBpm = beatMap.m_baseMapMetadata.preference_bpm > 0
                                ? beatMap.m_baseMapMetadata.preference_bpm
                                : 120.0;
        double lastTime   = 0;
        double lastBeat   = 0;

        // Find the first BPM timing point to use as origin
        for ( const auto& t : beatMap.m_timings ) {
            if ( t.m_timingEffect == TimingEffect::BPM ) {
                lastTime   = t.m_timestamp;
                currentBpm = t.m_bpm;

                // 从元数据中恢复原始 Beat
                if ( auto it = t.m_metadata.timing_properties.find(
                         TimingMetadataType::MALODY);
                     it != t.m_metadata.timing_properties.end() ) {
                    if ( it->second.contains("beat") ) {
                        try {
                            json bArr = json::parse(it->second.at("beat"));
                            if ( bArr.is_array() && bArr.size() >= 3 ) {
                                lastBeat = bArr[0].get<double>() +
                                           (bArr[1].get<double>() /
                                            bArr[2].get<double>());
                            }
                        } catch ( ... ) {
                        }
                    }
                }
                break;
            }
        }

        for ( const auto& t : beatMap.m_timings ) {
            if ( t.m_timingEffect != TimingEffect::BPM ) continue;
            if ( t.m_timestamp > time + 1e-4 ) break;
            if ( t.m_timestamp > lastTime + 1e-4 ) {
                lastBeat += (t.m_timestamp - lastTime) / (60000.0 / currentBpm);
                lastTime = t.m_timestamp;
            }
            currentBpm = t.m_bpm;
        }
        lastBeat += (time - lastTime) / (60000.0 / currentBpm);

        int    integerBeat = static_cast<int>(std::floor(lastBeat + 1e-6));
        double fraction    = lastBeat - integerBeat;

        if ( fraction < 1e-6 ) return json::array({ integerBeat, 0, 1 });
        if ( fraction > 1.0 - 1e-6 )
            return json::array({ integerBeat + 1, 0, 1 });

        // 尝试常见分母拟合，寻找最简约分
        for ( int den :
              { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 192, 1920 } ) {
            double num     = fraction * den;
            double rounded = std::round(num);
            if ( std::abs(num - rounded) < 1e-4 ) {
                int n   = static_cast<int>(rounded);
                int gcd = std::gcd(n, den);
                return json::array({ integerBeat, n / gcd, den / gcd });
            }
        }

        // 兜底方案
        int n   = static_cast<int>(std::round(fraction * 1920));
        int gcd = std::gcd(n, 1920);
        return json::array({ integerBeat, n / gcd, 1920 / gcd });
    };

    // 获取 audioOffset 和 initialDelay（优先从元数据中恢复）
    double audioOffset  = 0.0;
    double initialDelay = 0.0;
    if ( auto it =
             beatMap.m_metadata.map_properties.find(MapMetadataType::MALODY);
         it != beatMap.m_metadata.map_properties.end() ) {
        if ( it->second.contains("initialDelay") ) {
            try {
                initialDelay = std::stod(it->second.at("initialDelay"));
            } catch ( ... ) {
            }
        }
        if ( it->second.contains("audioOffset") ) {
            try {
                audioOffset = std::stod(it->second.at("audioOffset"));
            } catch ( ... ) {
            }
        }
    }

    // Time & Effect
    json timeArr = json::array();
    for ( const auto& t : beatMap.m_timings ) {
        if ( t.m_timingEffect == TimingEffect::BPM ) {
            json tj;

            bool hasBeat = false;
            if ( auto it = t.m_metadata.timing_properties.find(
                     TimingMetadataType::MALODY);
                 it != t.m_metadata.timing_properties.end() ) {
                if ( it->second.contains("beat") ) {
                    try {
                        tj["beat"] = json::parse(it->second.at("beat"));
                        hasBeat    = true;
                    } catch ( ... ) {
                    }
                }
            }
            if ( !hasBeat ) {
                tj["beat"] = timeToBeat(t.m_timestamp);
            }

            tj["bpm"] = t.m_bpm;

            // 恢复 Malody 特有字段
            if ( auto it = t.m_metadata.timing_properties.find(
                     TimingMetadataType::MALODY);
                 it != t.m_metadata.timing_properties.end() ) {
                for ( const auto& [key, val] : it->second ) {
                    if ( key != "bpm" ) {  // 已经有 bpm 了，排除
                        try {
                            tj[key] = json::parse(val);
                        } catch ( ... ) {
                            tj[key] = val;
                        }
                    }
                }
            }
            timeArr.push_back(tj);
        }
    }
    if ( !timeArr.empty() ) {
        fileData["time"] = timeArr;
    }

    json effectArr = json::array();
    for ( const auto& t : beatMap.m_timings ) {
        if ( t.m_timingEffect == TimingEffect::SCROLL ) {
            json ej;

            bool hasBeat = false;
            if ( auto it = t.m_metadata.timing_properties.find(
                     TimingMetadataType::MALODY);
                 it != t.m_metadata.timing_properties.end() ) {
                if ( it->second.contains("beat") ) {
                    try {
                        ej["beat"] = json::parse(it->second.at("beat"));
                        hasBeat    = true;
                    } catch ( ... ) {
                    }
                }
            }
            if ( !hasBeat ) {
                ej["beat"] = timeToBeat(t.m_timestamp);
            }

            ej["scroll"] = t.m_timingEffectParameter;

            // 恢复 Malody 特有字段
            if ( auto it = t.m_metadata.timing_properties.find(
                     TimingMetadataType::MALODY);
                 it != t.m_metadata.timing_properties.end() ) {
                for ( const auto& [key, val] : it->second ) {
                    if ( key != "scroll" ) {
                        try {
                            ej[key] = json::parse(val);
                        } catch ( ... ) {
                            ej[key] = val;
                        }
                    }
                }
            }
            effectArr.push_back(ej);
        }
    }
    if ( !effectArr.empty() ) {
        fileData["effect"] = effectArr;
    }

    // 收集所有子物件的指针，用于去重
    std::set<const Note*> subNotePtrs;
    for ( const auto& poly : beatMap.m_noteData.polylines ) {
        for ( const auto& subNoteRef : poly.m_subNotes ) {
            subNotePtrs.insert(&subNoteRef.get());
        }
    }

    auto isSubNote = [&](const Note& note) {
        return subNotePtrs.count(&note) > 0;
    };

    auto serializeToMalody = [&](const Note& note) {
        json nj;

        bool hasBeat = false;
        if ( auto it =
                 note.m_metadata.note_properties.find(NoteMetadataType::MALODY);
             it != note.m_metadata.note_properties.end() ) {
            if ( it->second.contains("beat") ) {
                try {
                    nj["beat"] = json::parse(it->second.at("beat"));
                    hasBeat    = true;
                } catch ( ... ) {
                }
            }
        }

        if ( !hasBeat ) {
            nj["beat"] = timeToBeat(note.m_timestamp);
        }

        if ( mode == 7 ) {
            nj["x"] = columnToX((int)note.m_track);
            nj["w"] = defaultW;
        } else {
            nj["column"] = (int)note.m_track;
        }

        auto getRelBeat = [&](double targetTime, const json& rootBeatArr) {
            double rootBeatVal =
                rootBeatArr[0].get<double>() +
                (rootBeatArr[1].get<double>() / rootBeatArr[2].get<double>());
            auto   relBeatArr = timeToBeat(targetTime);
            double relBeatVal =
                relBeatArr[0].get<double>() +
                (relBeatArr[1].get<double>() / relBeatArr[2].get<double>()) -
                rootBeatVal;

            int    intRelBeat = static_cast<int>(std::floor(relBeatVal + 1e-6));
            double relFrac    = relBeatVal - intRelBeat;

            if ( relFrac < 1e-6 ) return json::array({ intRelBeat, 0, 1 });

            for ( int den : { 1,
                              2,
                              3,
                              4,
                              6,
                              8,
                              12,
                              16,
                              24,
                              32,
                              48,
                              64,
                              96,
                              192,
                              1920 } ) {
                double num     = relFrac * den;
                double rounded = std::round(num);
                if ( std::abs(num - rounded) < 1e-4 ) {
                    int n   = static_cast<int>(rounded);
                    int gcd = std::gcd(n, den);
                    return json::array({ intRelBeat, n / gcd, den / gcd });
                }
            }
            int n   = static_cast<int>(std::round(relFrac * 1920));
            int gcd = std::gcd(n, 1920);
            return json::array({ intRelBeat, n / gcd, 1920 / gcd });
        };

        if ( note.m_type == NoteType::HOLD ) {
            const auto& h = static_cast<const Hold&>(note);

            std::string structure =
                (mode == 7 || mode == 4) ? "seg" : "endbeat";
            if ( auto it = note.m_metadata.note_properties.find(
                     NoteMetadataType::MALODY);
                 it != note.m_metadata.note_properties.end() ) {
                if ( it->second.contains("original_structure") ) {
                    structure = it->second.at("original_structure");
                }
            }

            if ( structure == "seg" ) {
                nj["seg"] = json::array();
                json sj;
                sj["beat"] =
                    getRelBeat(h.m_timestamp + h.m_duration, nj["beat"]);
                nj["seg"].push_back(sj);
            } else {
                nj["endbeat"] = timeToBeat(h.m_timestamp + h.m_duration);
            }
        } else if ( note.m_type == NoteType::FLICK ) {
            const auto& f = static_cast<const Flick&>(note);

            // 单 Flick 导出严格使用 dir 模式，忽略原始可能存在的 seg 模式
            // 模式 7 下：2 为右，8 为左（根据当前逻辑保留）
            nj["dir"] = (f.m_dtrack < 0) ? 8 : 2;
            int wVal  = defaultW + std::abs(f.m_dtrack);
            nj["w"]   = wVal;
        } else if ( note.m_type == NoteType::POLYLINE ) {
            const auto& p = static_cast<const Polyline&>(note);

            // 1. 预处理清洗：过滤 0 长度 Hold，合并同向 Flick
            struct CleanSeg {
                NoteType type;
                double   timestamp;
                double   duration;
                int      track;
                int      dtrack;
                const Note* original_sn;
            };
            std::vector<CleanSeg> cleanSubs;
            for ( const auto& subNoteRef : p.m_subNotes ) {
                const Note& sn = subNoteRef.get();
                if ( sn.m_type == NoteType::HOLD ) {
                    double dur = static_cast<const Hold&>(sn).m_duration;
                    if ( dur < 1e-4 ) continue;
                    cleanSubs.push_back({ NoteType::HOLD, sn.m_timestamp, dur, (int)sn.m_track, 0, &sn });
                } else if ( sn.m_type == NoteType::FLICK ) {
                    cleanSubs.push_back({ NoteType::FLICK, sn.m_timestamp, 0.0, (int)sn.m_track, static_cast<const Flick&>(sn).m_dtrack, &sn });
                }
            }

            // 迭代清洗与合并
            bool changed = true;
            while ( changed ) {
                changed = false;

                // 1. 过滤零值
                auto it = std::remove_if(cleanSubs.begin(), cleanSubs.end(), [](const auto& s) {
                    if ( s.type == NoteType::HOLD ) return s.duration < 1e-4;
                    if ( s.type == NoteType::FLICK ) return s.dtrack == 0;
                    return false;
                });
                if ( it != cleanSubs.end() ) {
                    cleanSubs.erase(it, cleanSubs.end());
                    changed = true;
                }

                // 2. 合并同类
                if ( cleanSubs.size() > 1 ) {
                    for ( size_t i = 0; i < cleanSubs.size() - 1; ) {
                        auto& curr = cleanSubs[i];
                        auto& next = cleanSubs[i + 1];
                        if ( curr.type == next.type ) {
                            if ( curr.type == NoteType::HOLD ) {
                                curr.duration += next.duration;
                                cleanSubs.erase(cleanSubs.begin() + i + 1);
                                changed = true;
                                continue;
                            } else if ( curr.type == NoteType::FLICK ) {
                                curr.dtrack += next.dtrack;
                                cleanSubs.erase(cleanSubs.begin() + i + 1);
                                changed = true;
                                continue;
                            }
                        }
                        i++;
                    }
                }
            }

            // 智能退化检查：如果折线物件实际上只有一个瞬时的滑键段，则导出为标准的 dir 模式
            bool exportedAsDir = false;
            if ( cleanSubs.size() == 1 ) {
                const auto& s = cleanSubs[0];
                if ( s.type == NoteType::FLICK &&
                     std::abs(s.timestamp - p.m_timestamp) < 1e-5 ) {
                    nj["dir"]     = (s.dtrack < 0) ? 8 : 2;
                    nj["w"]       = defaultW + std::abs(s.dtrack);
                    exportedAsDir = true;
                }
            }

            if ( !exportedAsDir ) {
                nj["seg"] = json::array();

                for ( size_t i = 0; i < cleanSubs.size(); ++i ) {
                    const auto& s = cleanSubs[i];

                    double   current_time  = s.timestamp;
                    uint32_t current_track = s.track;
                    if ( s.type == NoteType::HOLD ) {
                        current_time += s.duration;
                    } else if ( s.type == NoteType::FLICK ) {
                        current_track += s.dtrack;
                    }

                    json sj;
                    bool hasBeatMetadata = false;
                    if ( s.original_sn ) {
                        if ( auto it = s.original_sn->m_metadata.note_properties.find(
                                 NoteMetadataType::MALODY);
                             it != s.original_sn->m_metadata.note_properties.end() ) {
                            if ( it->second.contains("beat") ) {
                                try {
                                    sj["beat"] = json::parse(it->second.at("beat"));
                                    hasBeatMetadata = true;
                                } catch ( ... ) {
                                }
                            }
                        }
                    }

                    if ( !hasBeatMetadata ) {
                        sj["beat"] = getRelBeat(current_time, nj["beat"]);
                    }

                    float w        = 256.0f / trackCount;
                    int   x_offset = static_cast<int>(
                        std::round((int(current_track) - int(p.m_track)) * w));
                    if ( x_offset != 0 ) {
                        sj["x"] = x_offset;
                    }

                    // Restore other segment properties from metadata
                    if ( s.original_sn ) {
                        if ( auto it = s.original_sn->m_metadata.note_properties.find(
                                 NoteMetadataType::MALODY);
                             it != s.original_sn->m_metadata.note_properties.end() ) {
                            for ( const auto& [key, val] : it->second ) {
                                if ( key != "beat" && key != "x" ) {
                                    try {
                                        sj[key] = json::parse(val);
                                    } catch ( ... ) {
                                        sj[key] = val;
                                    }
                                }
                            }
                        }
                    }

                    nj["seg"].push_back(sj);
                }
            }
        }
        if ( auto it =
                 note.m_metadata.note_properties.find(NoteMetadataType::MALODY);
             it != note.m_metadata.note_properties.end() ) {
            for ( const auto& [key, val] : it->second ) {
                // 排除已由程序逻辑确定的核心字段，防止旧元数据覆盖新计算结果
                if ( key != "beat" && key != "column" && key != "endbeat" &&
                     key != "seg" && key != "dir" &&
                     (note.m_type != NoteType::FLICK || key != "w") ) {
                    try {
                        nj[key] = json::parse(val);
                    } catch ( ... ) {
                        nj[key] = val;
                    }
                }
            }
        }
        return nj;
    };

    auto& noteArr = fileData["note"];
    noteArr       = json::array();

    // 插入音频节点
    json audioNode;
    audioNode["beat"] = json::array({ 0, 0, 1 });
    audioNode["sound"] =
        beatMap.m_baseMapMetadata.main_audio_path.filename().string();
    audioNode["type"] = 1;

    double firstTimingTime = 0.0;
    for ( const auto& t : beatMap.m_timings ) {
        if ( t.m_timingEffect == TimingEffect::BPM ) {
            firstTimingTime = t.m_timestamp;
            // 如果元数据里没有，尝试从第一个 BPM 点的 delay 恢复 initialDelay
            if ( initialDelay == 0.0 ) {
                if ( auto it2 = t.m_metadata.timing_properties.find(
                         TimingMetadataType::MALODY);
                     it2 != t.m_metadata.timing_properties.end() ) {
                    if ( it2->second.contains("delay") ) {
                        try {
                            initialDelay = std::stod(it2->second.at("delay"));
                        } catch ( ... ) {
                        }
                    }
                }
            }
            break;
        }
    }

    // 如果仍然找不到 audioOffset，则使用推导值（假设 initialDelay 对应的
    // timestamp 是 firstTimingTime）
    if ( audioOffset == 0.0 && firstTimingTime != 0.0 ) {
        audioOffset = initialDelay - firstTimingTime;
    }

    audioNode["offset"] = static_cast<int64_t>(std::round(audioOffset));

    noteArr.push_back(audioNode);

    std::vector<const Note*> sortedNotes;
    for ( const auto& n : beatMap.m_noteData.notes )
        if ( !isSubNote(n) ) sortedNotes.push_back(&n);
    for ( const auto& n : beatMap.m_noteData.holds )
        if ( !isSubNote(n) ) sortedNotes.push_back(&n);
    for ( const auto& n : beatMap.m_noteData.flicks )
        if ( !isSubNote(n) ) sortedNotes.push_back(&n);
    for ( const auto& poly : beatMap.m_noteData.polylines )
        if ( !isSubNote(poly) ) sortedNotes.push_back(&poly);

    std::sort(sortedNotes.begin(), sortedNotes.end(),
              [](const Note* a, const Note* b) {
                  if ( std::abs(a->m_timestamp - b->m_timestamp) > 1e-6 )
                      return a->m_timestamp < b->m_timestamp;
                  return a->m_track < b->m_track;
              });

    for ( const auto* n : sortedNotes ) {
        noteArr.push_back(serializeToMalody(*n));
    }

    std::ofstream ofs(path);
    if ( !ofs.is_open() ) return false;
    ofs << fileData.dump(4);
    return true;
}

}  // namespace MMM
