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
    meta["cover"]  = "";
    meta["id"]     = 0;
    meta["aimode"] = "";
    meta["mode"]   = 7;  // 默认使用坐标模式，与 Malody 编辑器一致

    auto& song        = meta["song"];
    song["title"]     = beatMap.m_baseMapMetadata.title;
    song["titleorg"]  = beatMap.m_baseMapMetadata.title_unicode;
    song["artist"]    = beatMap.m_baseMapMetadata.artist;
    song["artistorg"] = beatMap.m_baseMapMetadata.artist_unicode;
    song["file"] =
        beatMap.m_baseMapMetadata.main_audio_path.filename().string();
    song["bpm"] = beatMap.m_baseMapMetadata.preference_bpm;

    auto& mode_ext        = meta["mode_ext"];
    mode_ext["bar_begin"] = 0;
    if ( trackCount > 0 ) {
        mode_ext["column"] = trackCount;
    }

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

            if ( !tj.contains("delay") ) {
                tj["delay"] = t.m_timestamp;
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

            std::string structure = "dir";
            if ( auto it = note.m_metadata.note_properties.find(
                     NoteMetadataType::MALODY);
                 it != note.m_metadata.note_properties.end() ) {
                if ( it->second.contains("original_structure") ) {
                    structure = it->second.at("original_structure");
                } else if ( it->second.contains("original_structure_flick") ) {
                    structure = it->second.at("original_structure_flick");
                }
            }

            if ( structure == "seg" ) {
                nj["seg"] = json::array();
                json sj;
                sj["beat"] = json::array({ 0, 0, 1 });
                sj["x"] = static_cast<int>(std::round(f.m_dtrack * defaultW));
                nj["seg"].push_back(sj);
            } else {
                nj["dir"] = (f.m_dtrack < 0) ? 8 : 2;
                int wVal  = defaultW + std::abs(f.m_dtrack);
                nj["w"]   = wVal;
            }
        } else if ( note.m_type == NoteType::POLYLINE ) {
            const auto& p = static_cast<const Polyline&>(note);
            nj["seg"]     = json::array();

            for ( size_t i = 0; i < p.m_subNotes.size(); ++i ) {
                const auto& sn = p.m_subNotes[i].get();

                double   current_time  = sn.m_timestamp;
                uint32_t current_track = sn.m_track;
                if ( sn.m_type == NoteType::HOLD ) {
                    current_time += static_cast<const Hold&>(sn).m_duration;
                } else if ( sn.m_type == NoteType::FLICK ) {
                    current_track += static_cast<const Flick&>(sn).m_dtrack;
                }

                // Look ahead for instant Flick to merge
                if ( i + 1 < p.m_subNotes.size() ) {
                    const auto& next_sn = p.m_subNotes[i + 1].get();
                    if ( next_sn.m_type == NoteType::FLICK &&
                         std::abs(next_sn.m_timestamp - current_time) < 1e-5 ) {
                        current_track =
                            next_sn.m_track +
                            static_cast<const Flick&>(next_sn).m_dtrack;
                        i++;
                    }
                }

                json sj;
                bool hasBeatMetadata = false;
                if ( auto it = sn.m_metadata.note_properties.find(
                         NoteMetadataType::MALODY);
                     it != sn.m_metadata.note_properties.end() ) {
                    if ( it->second.contains("beat") ) {
                        try {
                            sj["beat"] = json::parse(it->second.at("beat"));
                            hasBeatMetadata = true;
                        } catch ( ... ) {
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
                if ( auto it = sn.m_metadata.note_properties.find(
                         NoteMetadataType::MALODY);
                     it != sn.m_metadata.note_properties.end() ) {
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

                nj["seg"].push_back(sj);
            }
        }
        if ( auto it =
                 note.m_metadata.note_properties.find(NoteMetadataType::MALODY);
             it != note.m_metadata.note_properties.end() ) {
            for ( const auto& [key, val] : it->second ) {
                if ( key != "beat" && key != "column" && key != "endbeat" &&
                     key != "seg" ) {
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

    audioNode["offset"] = audioOffset;

    audioNode["vol"] = 100;
    noteArr.push_back(audioNode);

    auto process = [&](auto& deque) {
        for ( const auto& n : deque ) {
            if ( !isSubNote(n) ) noteArr.push_back(serializeToMalody(n));
        }
    };
    process(beatMap.m_noteData.notes);
    process(beatMap.m_noteData.holds);
    process(beatMap.m_noteData.flicks);
    for ( const auto& poly : beatMap.m_noteData.polylines )
        noteArr.push_back(serializeToMalody(poly));

    fileData["extra"] = json::object();

    std::ofstream ofs(path);
    if ( !ofs.is_open() ) return false;
    ofs << fileData.dump(4);
    return true;
}

}  // namespace MMM
