#pragma once

#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>

namespace MMM
{

using json = nlohmann::json;

/**
 * @brief 从 mmm 格式 (JSON) 加载谱面
 * @param path 谱面文件路径
 * @return 加载完成的谱面对象
 */
inline BeatMap loadMMMMap(const std::filesystem::path& path)
{
    BeatMap       beatMap;
    std::ifstream file(path);
    if ( !file.is_open() ) {
        XERROR("Failed to open mmm map file: {}", path.string());
        return beatMap;
    }

    json root;
    try {
        file >> root;
    } catch ( const std::exception& e ) {
        XERROR("Failed to parse mmm map JSON: {}", e.what());
        return beatMap;
    }

    beatMap.m_baseMapMetadata.map_path = path;

    // Helper to load note metadata
    auto loadNoteMetadata = [](Note& note, const json& nJson) {
        if ( nJson.contains("extra") ) {
            for ( const auto& extraItem : nJson["extra"] ) {
                for ( auto it = extraItem.begin(); it != extraItem.end();
                      ++it ) {
                    NoteMetadataType mtype;
                    if ( it.key() == "osu" )
                        mtype = NoteMetadataType::OSU;
                    else if ( it.key() == "malody" )
                        mtype = NoteMetadataType::MALODY;
                    else
                        continue;
                    auto& props = note.m_metadata.note_properties[mtype];
                    for ( auto propIt = it.value().begin();
                          propIt != it.value().end();
                          ++propIt ) {
                        props[propIt.key()] = propIt.value().get<std::string>();
                    }
                }
            }
        }
    };

    // 1. Metadata
    if ( root.contains("metadata") ) {
        const auto& metadata = root["metadata"];
        if ( metadata.contains("base") ) {
            const auto& base                = metadata["base"];
            beatMap.m_baseMapMetadata.name  = base.value("name", "");
            beatMap.m_baseMapMetadata.title = base.value("title", "");
            beatMap.m_baseMapMetadata.title_unicode =
                base.value("title_unicode", "");
            beatMap.m_baseMapMetadata.artist = base.value("artist", "");
            beatMap.m_baseMapMetadata.artist_unicode =
                base.value("artist_unicode", "");
            beatMap.m_baseMapMetadata.version = base.value("version", "");
            beatMap.m_baseMapMetadata.author  = base.value("author", "");
            beatMap.m_baseMapMetadata.main_audio_path = base.value("audio", "");
            beatMap.m_baseMapMetadata.main_cover_path = base.value("cover", "");
            beatMap.m_baseMapMetadata.track_count =
                base.value("track_count", 4);
            beatMap.m_baseMapMetadata.preference_bpm = base.value("bpm", 120.0);
            beatMap.m_baseMapMetadata.map_length = base.value("duration", 0.0);
        }

        if ( metadata.contains("extra") ) {
            for ( const auto& extraItem : metadata["extra"] ) {
                for ( auto it = extraItem.begin(); it != extraItem.end();
                      ++it ) {
                    MapMetadataType type;
                    if ( it.key() == "osu" )
                        type = MapMetadataType::OSU;
                    else if ( it.key() == "malody" )
                        type = MapMetadataType::MALODY;
                    else if ( it.key() == "rm" )
                        type = MapMetadataType::RM;
                    else
                        continue;

                    auto& props = beatMap.m_metadata.map_properties[type];
                    for ( auto propIt = it.value().begin();
                          propIt != it.value().end();
                          ++propIt ) {
                        props[propIt.key()] = propIt.value().get<std::string>();
                    }
                }
            }
        }
    }

    // 2. Timing
    if ( root.contains("timing") ) {
        for ( const auto& tJson : root["timing"] ) {
            Timing t;
            t.m_timestamp             = tJson.value("timestamp", 0.0);
            t.m_bpm                   = tJson.value("bpm", 120.0);
            t.m_beat_length           = tJson.value("beat_length", 500.0);
            t.m_timingEffect          = tJson.value("effect", "bpm") == "bpm"
                                            ? TimingEffect::BPM
                                            : TimingEffect::SCROLL;
            t.m_timingEffectParameter = tJson.value("param", 0.0);

            if ( tJson.contains("extra") ) {
                for ( const auto& extraItem : tJson["extra"] ) {
                    for ( auto it = extraItem.begin(); it != extraItem.end();
                          ++it ) {
                        TimingMetadataType type;
                        if ( it.key() == "osu" )
                            type = TimingMetadataType::OSU;
                        else if ( it.key() == "malody" )
                            type = TimingMetadataType::MALODY;
                        else
                            continue;

                        auto& props = t.m_metadata.timing_properties[type];
                        for ( auto propIt = it.value().begin();
                              propIt != it.value().end();
                              ++propIt ) {
                            props[propIt.key()] =
                                propIt.value().get<std::string>();
                        }
                    }
                }
            }
            beatMap.m_timings.push_back(t);
        }
    }

    // 3. Note
    if ( root.contains("note") ) {
        // 预扫描所有折线子物件引用，用于去重（防御旧版本残留的重复数据）
        std::set<std::pair<double, uint32_t>> subNoteKeys;
        for ( const auto& nJson : root["note"] ) {
            if ( nJson.value("type", "note") == "polyline" ) {
                if ( nJson.contains("sub_notes") ) {
                    for ( const auto& snJson : nJson["sub_notes"] ) {
                        subNoteKeys.insert({ snJson.value("timestamp", 0.0),
                                             snJson.value("track", 0) });
                    }
                }
            }
        }

        for ( const auto& nJson : root["note"] ) {
            std::string type = nJson.value("type", "note");
            if ( type == "polyline" ) {
                Polyline& poly   = beatMap.m_noteData.polylines.emplace_back();
                poly.m_type      = NoteType::POLYLINE;
                poly.m_timestamp = nJson.value("timestamp", 0.0);
                poly.m_track     = nJson.value("track", 0);

                loadNoteMetadata(poly, nJson);

                if ( nJson.contains("sub_notes") ) {
                    for ( const auto& snJson : nJson["sub_notes"] ) {
                        std::string stype = snJson.value("type", "note");
                        if ( stype == "hold" ) {
                            Hold& h  = beatMap.m_noteData.holds.emplace_back();
                            h.m_type = NoteType::HOLD;
                            h.m_timestamp = snJson.value("timestamp", 0.0);
                            h.m_track     = snJson.value("track", 0);
                            h.m_duration  = snJson.value("duration", 0.0);
                            poly.m_subNotes.push_back(h);
                            poly.m_subHolds.push_back(h);
                            beatMap.m_allNotes.push_back(h);
                        } else if ( stype == "flick" ) {
                            Flick& f = beatMap.m_noteData.flicks.emplace_back();
                            f.m_type = NoteType::FLICK;
                            f.m_timestamp = snJson.value("timestamp", 0.0);
                            f.m_track     = snJson.value("track", 0);
                            f.m_dtrack    = snJson.value("dtrack", 0);
                            poly.m_subNotes.push_back(f);
                            poly.m_subFlicks.push_back(f);
                            beatMap.m_allNotes.push_back(f);
                        }
                    }
                }
                beatMap.m_allNotes.push_back(poly);
            } else if ( type == "hold" ) {
                // 跳过属于折线子物件的独立条目（防御旧版本残留的重复数据）
                if ( subNoteKeys.count({ nJson.value("timestamp", 0.0),
                                         nJson.value("track", 0) }) ) {
                    continue;
                }
                Hold& h       = beatMap.m_noteData.holds.emplace_back();
                h.m_type      = NoteType::HOLD;
                h.m_timestamp = nJson.value("timestamp", 0.0);
                h.m_track     = nJson.value("track", 0);
                h.m_duration  = nJson.value("duration", 0.0);
                loadNoteMetadata(h, nJson);
                beatMap.m_allNotes.push_back(h);
            } else if ( type == "flick" ) {
                // 跳过属于折线子物件的独立条目（防御旧版本残留的重复数据）
                if ( subNoteKeys.count({ nJson.value("timestamp", 0.0),
                                         nJson.value("track", 0) }) ) {
                    continue;
                }
                Flick& f      = beatMap.m_noteData.flicks.emplace_back();
                f.m_type      = NoteType::FLICK;
                f.m_timestamp = nJson.value("timestamp", 0.0);
                f.m_track     = nJson.value("track", 0);
                f.m_dtrack    = nJson.value("dtrack", 0);
                loadNoteMetadata(f, nJson);
                beatMap.m_allNotes.push_back(f);
            } else {
                // 跳过属于折线子物件的独立条目（防御旧版本残留的重复数据）
                if ( subNoteKeys.count({ nJson.value("timestamp", 0.0),
                                         nJson.value("track", 0) }) ) {
                    continue;
                }
                Note& n       = beatMap.m_noteData.notes.emplace_back();
                n.m_type      = NoteType::NOTE;
                n.m_timestamp = nJson.value("timestamp", 0.0);
                n.m_track     = nJson.value("track", 0);
                loadNoteMetadata(n, nJson);
                beatMap.m_allNotes.push_back(n);
            }
        }
    }

    return beatMap;
}

}  // namespace MMM
