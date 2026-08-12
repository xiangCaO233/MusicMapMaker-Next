#pragma once

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/Timing.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <vector>

namespace MMM
{

using json = nlohmann::json;

/**
 * @brief 保存谱面为 mmm 格式 (JSON)
 * @param beatMap 谱面对象
 * @param path 保存路径
 * @return 是否保存成功
 */
inline bool saveMMMMap(const BeatMap&               beatMap,
                       const std::filesystem::path& path)
{
    json root;
    root["format_version"] = 2;

    // 1. 元数据。
    auto& metadata = root["metadata"];

    // 基础元数据。
    auto& base             = metadata["base"];
    base["name"]           = beatMap.m_baseMapMetadata.name;
    base["title"]          = beatMap.m_baseMapMetadata.title;
    base["title_unicode"]  = beatMap.m_baseMapMetadata.title_unicode;
    base["artist"]         = beatMap.m_baseMapMetadata.artist;
    base["artist_unicode"] = beatMap.m_baseMapMetadata.artist_unicode;
    base["version"]        = beatMap.m_baseMapMetadata.version;
    base["author"]         = beatMap.m_baseMapMetadata.author;
    base["song_file_hint"] =
        Config::pathToUtf8(beatMap.m_baseMapMetadata.song_file_hint);
    base["cover"] =
        Config::pathToUtf8(beatMap.m_baseMapMetadata.main_cover_path);
    base["cover_img"] =
        Config::pathToUtf8(beatMap.m_baseMapMetadata.cover_path);
    base["cover_type"] = static_cast<int>(beatMap.m_baseMapMetadata.cover_type);
    base["video_starttime"] = beatMap.m_baseMapMetadata.video_starttime;
    base["bgxoffset"]       = beatMap.m_baseMapMetadata.bgxoffset;
    base["bgyoffset"]       = beatMap.m_baseMapMetadata.bgyoffset;
    base["track_count"]     = beatMap.m_baseMapMetadata.track_count;
    base["bgm_track_count"] = beatMap.m_baseMapMetadata.bgm_track_count;
    base["bpm"]             = beatMap.m_baseMapMetadata.preference_bpm;
    base["duration"]        = beatMap.m_baseMapMetadata.map_length;

    // 来源格式附加元数据。
    auto& extra = metadata["extra"];
    extra       = json::array();
    for ( const auto& [type, props] : beatMap.m_metadata.map_properties ) {
        json        sourceObj;
        std::string sourceName;
        if ( type == MapMetadataType::OSU )
            sourceName = "osu";
        else if ( type == MapMetadataType::MALODY )
            sourceName = "malody";
        else if ( type == MapMetadataType::RM )
            sourceName = "rm";

        if ( sourceName.empty() ) continue;

        json propObj;
        for ( const auto& [key, val] : props ) {
            propObj[key] = val;
        }
        sourceObj[sourceName] = propObj;
        extra.push_back(sourceObj);
    }

    // 2. Timing 事件。
    auto& timingArr = root["timing"];
    timingArr       = json::array();
    for ( const auto& timing : beatMap.m_timings ) {
        json t;
        t["timestamp"]   = timing.m_timestamp;
        t["bpm"]         = timing.m_bpm;
        t["beat_length"] = timing.m_beat_length;
        t["effect"]      = timingEffectToString(timing.m_timingEffect);
        t["param"]       = timing.m_timingEffectParameter;

        auto& tExtra = t["extra"];
        tExtra       = json::array();
        for ( const auto& [type, props] :
              timing.m_metadata.timing_properties ) {
            json        sourceObj;
            std::string sourceName;
            if ( type == TimingMetadataType::OSU )
                sourceName = "osu";
            else if ( type == TimingMetadataType::MALODY )
                sourceName = "malody";

            if ( sourceName.empty() ) continue;

            json propObj;
            for ( const auto& [key, val] : props ) {
                propObj[key] = val;
            }
            sourceObj[sourceName] = propObj;
            tExtra.push_back(sourceObj);
        }
        timingArr.push_back(t);
    }

    // 3. 自动采样对象。
    auto& sampleArr = root["audio_samples"];
    sampleArr       = json::array();
    std::vector<const AudioSampleEvent*> sortedSamples;
    sortedSamples.reserve(beatMap.m_audioSamples.size());
    for ( const auto& sample : beatMap.m_audioSamples ) {
        sortedSamples.push_back(&sample);
    }
    std::stable_sort(
        sortedSamples.begin(),
        sortedSamples.end(),
        [](const AudioSampleEvent* lhs, const AudioSampleEvent* rhs) {
            if ( std::abs(lhs->m_timestamp - rhs->m_timestamp) > 1e-6 ) {
                return lhs->m_timestamp < rhs->m_timestamp;
            }
            if ( lhs->m_track != rhs->m_track ) {
                return lhs->m_track < rhs->m_track;
            }
            if ( lhs->m_audioResourceId != rhs->m_audioResourceId ) {
                return lhs->m_audioResourceId < rhs->m_audioResourceId;
            }
            return lhs->m_offsetMs < rhs->m_offsetMs;
        });
    for ( const AudioSampleEvent* sample : sortedSamples ) {
        json sampleJson;
        sampleJson["timestamp"] = sample->m_timestamp;
        sampleJson["offset_ms"] = sample->m_offsetMs;
        sampleJson["track"]     = sample->m_track;
        sampleJson["audio_ref"] = sample->m_audioResourceId;
        sampleJson["volume"]    = sample->m_volume;

        auto& sampleExtra = sampleJson["extra"];
        sampleExtra       = json::array();
        for ( const auto& [type, props] :
              sample->m_metadata.sample_properties ) {
            std::string sourceName;
            if ( type == SampleMetadataType::MALODY )
                sourceName = "malody";
            else if ( type == SampleMetadataType::MMM )
                sourceName = "mmm";

            if ( sourceName.empty() ) continue;

            json propertyObject;
            for ( const auto& [key, value] : props ) {
                propertyObject[key] = value;
            }
            json sourceObject;
            sourceObject[sourceName] = propertyObject;
            sampleExtra.push_back(sourceObject);
        }
        sampleArr.push_back(sampleJson);
    }

    // 4. 玩家物件。
    auto& noteArr = root["note"];
    noteArr       = json::array();

    // 将单个音符序列化为 JSON。
    auto serializeNote = [&](const Note& note) {
        json n;
        n["timestamp"] = note.m_timestamp;
        n["track"]     = note.m_track;
        if ( !note.m_annotation.empty() ) {
            n["annotation"] = note.m_annotation;
        }
        if ( const auto binding = note.getSampleBinding() ) {
            n["sample"] = { { "audio_ref", binding->m_audioResourceId },
                            { "volume", binding->m_volume } };
        }

        switch ( note.m_type ) {
        case NoteType::NOTE: n["type"] = "note"; break;
        case NoteType::HOLD: {
            n["type"]     = "hold";
            n["duration"] = static_cast<const Hold&>(note).m_duration;
            break;
        }
        case NoteType::FLICK: {
            n["type"]   = "flick";
            n["dtrack"] = static_cast<const Flick&>(note).m_dtrack;
            break;
        }
        case NoteType::POLYLINE: {
            const auto& poly = static_cast<const Polyline&>(note);

            const bool preserveAnnotatedStructure =
                !poly.m_annotation.empty() ||
                std::any_of(poly.m_subNotes.begin(),
                            poly.m_subNotes.end(),
                            [](const auto& subNote) {
                                return !subNote.get().m_annotation.empty();
                            });
            if ( preserveAnnotatedStructure ) {
                n["type"]         = "polyline";
                json subNotesJson = json::array();
                for ( const auto& subNoteRef : poly.m_subNotes ) {
                    const Note& subNote = subNoteRef.get();
                    json        subJson;
                    subJson["timestamp"] = subNote.m_timestamp;
                    subJson["track"]     = subNote.m_track;
                    if ( subNote.m_type == NoteType::HOLD ) {
                        subJson["type"] = "hold";
                        subJson["duration"] =
                            static_cast<const Hold&>(subNote).m_duration;
                    } else if ( subNote.m_type == NoteType::FLICK ) {
                        subJson["type"] = "flick";
                        subJson["dtrack"] =
                            static_cast<const Flick&>(subNote).m_dtrack;
                    } else {
                        subJson["type"] = "note";
                    }
                    if ( const auto binding = subNote.getSampleBinding() ) {
                        subJson["sample"] = { { "audio_ref",
                                                binding->m_audioResourceId },
                                              { "volume", binding->m_volume } };
                    }
                    if ( !subNote.m_annotation.empty() ) {
                        subJson["annotation"] = subNote.m_annotation;
                    }
                    subNotesJson.push_back(std::move(subJson));
                }
                n["sub_notes"] = std::move(subNotesJson);
                break;
            }

            // 1. 预处理清洗：过滤 0 长度 Hold，合并同向 Flick
            struct CleanSeg {
                NoteType type;
                double   timestamp;
                double   duration;
                int      track;
                int      dtrack;
                /// @brief 清洗过程中随子物件保留的命中采样绑定。
                std::optional<AudioSampleBinding> sampleBinding;
            };
            std::vector<CleanSeg> cleanSubs;
            for ( const auto& subNoteRef : poly.m_subNotes ) {
                const Note& sn = subNoteRef.get();
                if ( sn.m_type == NoteType::HOLD ) {
                    double dur = static_cast<const Hold&>(sn).m_duration;
                    if ( dur < 1e-4 ) continue;
                    cleanSubs.push_back({ NoteType::HOLD,
                                          sn.m_timestamp,
                                          dur,
                                          (int)sn.m_track,
                                          0,
                                          sn.getSampleBinding() });
                } else if ( sn.m_type == NoteType::FLICK ) {
                    cleanSubs.push_back(
                        { NoteType::FLICK,
                          sn.m_timestamp,
                          0.0,
                          (int)sn.m_track,
                          static_cast<const Flick&>(sn).m_dtrack,
                          sn.getSampleBinding() });
                }
            }

            // 迭代合并与清洗
            bool changed = true;
            while ( changed ) {
                changed = false;

                // 1. 过滤零值
                auto it = std::remove_if(
                    cleanSubs.begin(), cleanSubs.end(), [](const auto& s) {
                        if ( s.type == NoteType::HOLD )
                            return s.duration < 1e-4;
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
                                if ( !curr.sampleBinding ) {
                                    curr.sampleBinding = next.sampleBinding;
                                }
                                cleanSubs.erase(cleanSubs.begin() + i + 1);
                                changed = true;
                                continue;
                            } else if ( curr.type == NoteType::FLICK ) {
                                curr.dtrack += next.dtrack;
                                if ( !curr.sampleBinding ) {
                                    curr.sampleBinding = next.sampleBinding;
                                }
                                cleanSubs.erase(cleanSubs.begin() + i + 1);
                                changed = true;
                                continue;
                            }
                        }
                        i++;
                    }
                }
            }

            // 2. 根据清洗后的结果进行写出
            if ( cleanSubs.empty() ) {
                n["type"] = "note";
            } else if ( cleanSubs.size() == 1 ) {
                const auto& s = cleanSubs[0];
                if ( s.type == NoteType::HOLD ) {
                    n["type"]     = "hold";
                    n["duration"] = s.duration;
                } else if ( s.type == NoteType::FLICK ) {
                    n["type"]   = "flick";
                    n["dtrack"] = s.dtrack;
                } else {
                    n["type"] = "note";
                }
                n["timestamp"] = s.timestamp;
                n["track"]     = s.track;
                if ( !note.getSampleBinding() && s.sampleBinding ) {
                    n["sample"] = { { "audio_ref",
                                      s.sampleBinding->m_audioResourceId },
                                    { "volume", s.sampleBinding->m_volume } };
                }
            } else {
                n["type"]         = "polyline";
                json subNotesJson = json::array();
                for ( const auto& s : cleanSubs ) {
                    json snj;
                    snj["timestamp"] = s.timestamp;
                    snj["track"]     = s.track;
                    if ( s.type == NoteType::HOLD ) {
                        snj["type"]     = "hold";
                        snj["duration"] = s.duration;
                    } else if ( s.type == NoteType::FLICK ) {
                        snj["type"]   = "flick";
                        snj["dtrack"] = s.dtrack;
                    }
                    if ( s.sampleBinding ) {
                        snj["sample"] = {
                            { "audio_ref", s.sampleBinding->m_audioResourceId },
                            { "volume", s.sampleBinding->m_volume }
                        };
                    }
                    subNotesJson.push_back(snj);
                }
                n["sub_notes"] = subNotesJson;
            }
            break;
        }
        }

        auto& nExtra = n["extra"];
        nExtra       = json::array();
        for ( const auto& [type, props] : note.m_metadata.note_properties ) {
            json        sourceObj;
            std::string sourceName;
            if ( type == NoteMetadataType::OSU )
                sourceName = "osu";
            else if ( type == NoteMetadataType::MALODY )
                sourceName = "malody";
            else if ( type == NoteMetadataType::MMM )
                sourceName = "mmm";

            if ( sourceName.empty() ) continue;

            json propObj;
            for ( const auto& [key, val] : props ) {
                propObj[key] = val;
            }
            sourceObj[sourceName] = propObj;
            nExtra.push_back(sourceObj);
        }
        return n;
    };

    // 只保存顶层音符；Polyline 子音符已在父 Polyline 中序列化。
    std::set<const Note*> subNotesSet;
    for ( const auto& poly : beatMap.m_noteData.polylines ) {
        for ( const auto& subNoteRef : poly.m_subNotes ) {
            subNotesSet.insert(&subNoteRef.get());
        }
    }

    std::vector<json> serializedNotes;
    for ( const auto& note : beatMap.m_noteData.notes ) {
        if ( subNotesSet.find(&note) == subNotesSet.end() )
            serializedNotes.push_back(serializeNote(note));
    }
    for ( const auto& hold : beatMap.m_noteData.holds ) {
        if ( subNotesSet.find(&hold) == subNotesSet.end() )
            serializedNotes.push_back(serializeNote(hold));
    }
    for ( const auto& flick : beatMap.m_noteData.flicks ) {
        if ( subNotesSet.find(&flick) == subNotesSet.end() )
            serializedNotes.push_back(serializeNote(flick));
    }
    for ( const auto& poly : beatMap.m_noteData.polylines ) {
        serializedNotes.push_back(serializeNote(poly));
    }

    // 按时间戳排序，保证写出稳定。
    std::sort(serializedNotes.begin(),
              serializedNotes.end(),
              [](const json& a, const json& b) {
                  return a["timestamp"].get<double>() <
                         b["timestamp"].get<double>();
              });

    for ( auto& n : serializedNotes ) {
        noteArr.push_back(n);
    }

    std::ofstream file(path);
    if ( !file.is_open() ) {
        XERROR("Failed to open file for saving mmm map: {}",
               Config::pathToUtf8(path));
        return false;
    }

    file << root.dump(4);
    return true;
}

}  // namespace MMM
