#pragma once

#include "log/colorful-log.h"
#include "mmm/SafeParse.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>

namespace MMM
{

using json = nlohmann::json;

inline bool saveMalodyMap(const BeatMap& beatMap, std::filesystem::path path)
{
    json fileData;

    // Meta
    auto& meta      = fileData["meta"];
    meta["creator"] = beatMap.m_baseMapMetadata.author;
    meta["version"] = beatMap.m_baseMapMetadata.version;
    meta["background"] =
        beatMap.m_baseMapMetadata.main_cover_path.filename().string();
    auto& song        = meta["song"];
    song["title"]     = beatMap.m_baseMapMetadata.title;
    song["titleorg"]  = beatMap.m_baseMapMetadata.title_unicode;
    song["artist"]    = beatMap.m_baseMapMetadata.artist;
    song["artistorg"] = beatMap.m_baseMapMetadata.artist_unicode;
    song["file"] =
        beatMap.m_baseMapMetadata.main_audio_path.filename().string();
    song["bpm"]        = beatMap.m_baseMapMetadata.preference_bpm;
    auto& mode_ext     = meta["mode_ext"];
    mode_ext["column"] = (int)beatMap.m_baseMapMetadata.track_count;

    if ( auto it =
             beatMap.m_metadata.map_properties.find(MapMetadataType::MALODY);
         it != beatMap.m_metadata.map_properties.end() ) {
        for ( const auto& [key, val] : it->second ) {
            if ( key == "id" || key == "preview" || key == "mode" ) {
                meta[key] = MMM::Internal::safeStoi(val);
            } else if ( key == "aimode" ) {
                meta[key] = val;
            } else if ( key == "mode_ext" ) {
                try {
                    meta["mode_ext"] = json::parse(val);
                } catch ( ... ) {
                }
            }
        }
    } else {
        meta["mode"] = 0;
    }

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
                break;
            }
        }

        for ( const auto& t : beatMap.m_timings ) {
            if ( t.m_timingEffect != TimingEffect::BPM ) continue;
            if ( t.m_timestamp > time + 0.5 ) break;
            lastBeat += (t.m_timestamp - lastTime) / (60000.0 / currentBpm);
            lastTime   = t.m_timestamp;
            currentBpm = t.m_bpm;
        }
        lastBeat += (time - lastTime) / (60000.0 / currentBpm);
        int    integerBeat = static_cast<int>(std::floor(lastBeat + 1e-6));
        double fraction    = lastBeat - integerBeat;
        return json::array({ integerBeat,
                             static_cast<int>(std::round(fraction * 1920)),
                             1920 });
    };

    // Time & Effect
    auto& timeArr = fileData["time"];
    timeArr       = json::array();
    for ( const auto& t : beatMap.m_timings ) {
        if ( t.m_timingEffect == TimingEffect::BPM ) {
            json tj;
            tj["beat"] = timeToBeat(t.m_timestamp);
            tj["bpm"]  = t.m_bpm;
            timeArr.push_back(tj);
        }
    }
    auto& effectArr = fileData["effect"];
    effectArr       = json::array();
    for ( const auto& t : beatMap.m_timings ) {
        if ( t.m_timingEffect == TimingEffect::SCROLL ) {
            json ej;
            ej["beat"]   = timeToBeat(t.m_timestamp);
            ej["scroll"] = t.m_timingEffectParameter;
            effectArr.push_back(ej);
        }
    }

    // 收集所有子物件，通过 (timestamp, track) 唯一标识
    struct NoteInfo {
        double time;
        int    track;
    };
    std::vector<NoteInfo> subNotes;
    for ( const auto& poly : beatMap.m_noteData.polylines ) {
        for ( const auto& subNoteRef : poly.m_subNotes ) {
            const auto& sn = subNoteRef.get();
            subNotes.push_back({ sn.m_timestamp, (int)sn.m_track });
        }
    }

    auto isSubNote = [&](const Note& note) {
        for ( const auto& sn : subNotes ) {
            if ( std::abs(sn.time - note.m_timestamp) < 5.0 &&
                 sn.track == (int)note.m_track )
                return true;
        }
        return false;
    };

    auto serializeToMalody = [&](const Note& note) {
        json nj;
        nj["beat"]   = timeToBeat(note.m_timestamp);
        nj["column"] = (int)note.m_track;
        if ( note.m_type == NoteType::HOLD ) {
            const auto& h = static_cast<const Hold&>(note);
            nj["endbeat"] = timeToBeat(h.m_timestamp + h.m_duration);
        } else if ( note.m_type == NoteType::POLYLINE ) {
            const auto& p = static_cast<const Polyline&>(note);
            nj["seg"]     = json::array();

            double rootBeatVal =
                timeToBeat(p.m_timestamp)[0].get<double>() +
                (timeToBeat(p.m_timestamp)[1].get<double>() / 1920.0);

            std::vector<std::pair<double, uint32_t>> targets;
            for ( size_t i = 0; i < p.m_subNotes.size(); ++i ) {
                const auto& sn = p.m_subNotes[i].get();
                if ( sn.m_type == NoteType::HOLD ) {
                    const auto& h             = static_cast<const Hold&>(sn);
                    double      current_time  = h.m_timestamp + h.m_duration;
                    uint32_t    current_track = h.m_track;

                    // Look ahead for instant Flick
                    if ( i + 1 < p.m_subNotes.size() ) {
                        const auto& next_sn = p.m_subNotes[i + 1].get();
                        if ( next_sn.m_type == NoteType::FLICK &&
                             std::abs(next_sn.m_timestamp - current_time) <
                                 1e-5 ) {
                            const auto& f = static_cast<const Flick&>(next_sn);
                            current_track = f.m_track + f.m_dtrack;
                            i++;  // Skip the flick
                        }
                    }
                    targets.push_back({ current_time, current_track });
                } else if ( sn.m_type == NoteType::FLICK ) {
                    const auto& f = static_cast<const Flick&>(sn);
                    targets.push_back(
                        { f.m_timestamp, f.m_track + f.m_dtrack });
                }
            }

            for ( const auto& target : targets ) {
                json   sj;
                double subBeatVal =
                    timeToBeat(target.first)[0].get<double>() +
                    (timeToBeat(target.first)[1].get<double>() / 1920.0);
                double relBeat = subBeatVal - rootBeatVal;
                int intRelBeat = static_cast<int>(std::floor(relBeat + 1e-6));
                double relFrac = relBeat - intRelBeat;
                sj["beat"] =
                    json::array({ intRelBeat,
                                  static_cast<int>(std::round(relFrac * 1920)),
                                  1920 });
                int x_offset = (int(target.second) - int(p.m_track)) *
                               43;  // Assume bestW=43
                if ( x_offset != 0 ) {
                    sj["x"] = x_offset;
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
            break;
        }
    }
    audioNode["offset"] = static_cast<int>(std::round(-firstTimingTime));

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

    std::ofstream ofs(path);
    if ( !ofs.is_open() ) return false;
    ofs << fileData.dump(4);
    return true;
}

}  // namespace MMM
