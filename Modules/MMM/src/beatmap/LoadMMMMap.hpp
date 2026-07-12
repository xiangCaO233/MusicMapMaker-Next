#pragma once

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/Timing.h"
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <utility>

namespace MMM
{

using json = nlohmann::json;

/// @brief 从 MMM JSON 对象读取字符串字段。
/// @param object JSON 对象。
/// @param key 字段名。
/// @param fallback 字段缺失或类型错误时的默认值。
/// @return 读取到的字符串或默认值。
inline std::string readMMMString(const json& object, const char* key,
                                 std::string fallback = {})
{
    if ( !object.is_object() ) return fallback;
    auto it = object.find(key);
    if ( it == object.end() || !it->is_string() ) return fallback;
    return it->get_ref<const std::string&>();
}

/// @brief 从 MMM JSON 对象读取浮点字段。
/// @param object JSON 对象。
/// @param key 字段名。
/// @param fallback 字段缺失或类型错误时的默认值。
/// @return 读取到的有限浮点数或默认值。
inline double readMMMDouble(const json& object, const char* key,
                            double fallback)
{
    if ( !object.is_object() ) return fallback;
    auto it = object.find(key);
    if ( it == object.end() || !it->is_number() ) return fallback;
    const double value = it->get<double>();
    return std::isfinite(value) ? value : fallback;
}

/// @brief 从 MMM JSON 对象读取整数字段。
/// @param object JSON 对象。
/// @param key 字段名。
/// @param fallback 字段缺失或类型错误时的默认值。
/// @return 读取到的整数或默认值。
inline int readMMMInt(const json& object, const char* key, int fallback)
{
    if ( !object.is_object() ) return fallback;
    auto it = object.find(key);
    if ( it == object.end() || !it->is_number() ) return fallback;

    const double value = it->get<double>();
    if ( !std::isfinite(value) ||
         value < static_cast<double>(std::numeric_limits<int>::min()) ||
         value > static_cast<double>(std::numeric_limits<int>::max()) ) {
        return fallback;
    }
    return static_cast<int>(value);
}

/// @brief 从 MMM JSON 对象读取非负轨道索引。
/// @param object JSON 对象。
/// @param key 字段名。
/// @param fallback 字段缺失或类型错误时的默认值。
/// @return 读取到的非负整数或默认值。
inline uint32_t readMMMU32(const json& object, const char* key,
                           uint32_t fallback)
{
    const int parsed = readMMMInt(object, key, static_cast<int>(fallback));
    if ( parsed < 0 ) return fallback;
    return static_cast<uint32_t>(parsed);
}

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
        XERROR("Failed to open mmm map file: {}", Config::pathToUtf8(path));
        return beatMap;
    }

    json root = json::parse(file, nullptr, false);
    if ( root.is_discarded() || !root.is_object() || file.bad() ) {
        XERROR("Failed to parse mmm map JSON: {}", Config::pathToUtf8(path));
        return beatMap;
    }

    beatMap.m_baseMapMetadata.map_path = path;

    /// @brief 从 MMM extra 对象中复制字符串属性。
    auto copyStringMetadataProperties = [](auto& props, const json& propsJson) {
        if ( !propsJson.is_object() ) return;
        for ( auto propIt = propsJson.begin(); propIt != propsJson.end();
              ++propIt ) {
            if ( propIt.value().is_string() ) {
                props[propIt.key()] =
                    propIt.value().template get_ref<const std::string&>();
            }
        }
    };

    // 加载音符元数据
    auto loadNoteMetadata = [copyStringMetadataProperties](Note&       note,
                                                           const json& nJson) {
        auto extraIt = nJson.find("extra");
        if ( extraIt == nJson.end() || !extraIt->is_array() ) return;
        for ( const auto& extraItem : *extraIt ) {
            if ( !extraItem.is_object() ) continue;
            for ( auto it = extraItem.begin(); it != extraItem.end(); ++it ) {
                NoteMetadataType mtype;
                if ( it.key() == "osu" )
                    mtype = NoteMetadataType::OSU;
                else if ( it.key() == "malody" )
                    mtype = NoteMetadataType::MALODY;
                else if ( it.key() == "mmm" )
                    mtype = NoteMetadataType::MMM;
                else
                    continue;
                auto& props = note.m_metadata.note_properties[mtype];
                copyStringMetadataProperties(props, it.value());
            }
        }
    };

    // 1. 元数据。
    auto metadataIt = root.find("metadata");
    if ( metadataIt != root.end() && metadataIt->is_object() ) {
        const auto& metadata = *metadataIt;
        if ( metadata.contains("base") ) {
            const auto& base                = metadata["base"];
            beatMap.m_baseMapMetadata.name  = readMMMString(base, "name");
            beatMap.m_baseMapMetadata.title = readMMMString(base, "title");
            beatMap.m_baseMapMetadata.title_unicode =
                readMMMString(base, "title_unicode");
            beatMap.m_baseMapMetadata.artist = readMMMString(base, "artist");
            beatMap.m_baseMapMetadata.artist_unicode =
                readMMMString(base, "artist_unicode");
            beatMap.m_baseMapMetadata.version = readMMMString(base, "version");
            beatMap.m_baseMapMetadata.author  = readMMMString(base, "author");
            beatMap.m_baseMapMetadata.main_audio_path =
                Config::utf8ToPath(readMMMString(base, "audio"));
            beatMap.m_baseMapMetadata.main_cover_path =
                Config::utf8ToPath(readMMMString(base, "cover"));
            beatMap.m_baseMapMetadata.cover_path =
                Config::utf8ToPath(readMMMString(base, "cover_img"));
            const int coverType = readMMMInt(
                base,
                "cover_type",
                static_cast<int>(beatMap.m_baseMapMetadata.cover_type));
            if ( coverType == static_cast<int>(CoverType::IMAGE) ||
                 coverType == static_cast<int>(CoverType::VIDEO) ) {
                beatMap.m_baseMapMetadata.cover_type =
                    static_cast<CoverType>(coverType);
            }
            beatMap.m_baseMapMetadata.video_starttime =
                readMMMInt(base,
                           "video_starttime",
                           beatMap.m_baseMapMetadata.video_starttime);
            beatMap.m_baseMapMetadata.bgxoffset = readMMMInt(
                base, "bgxoffset", beatMap.m_baseMapMetadata.bgxoffset);
            beatMap.m_baseMapMetadata.bgyoffset = readMMMInt(
                base, "bgyoffset", beatMap.m_baseMapMetadata.bgyoffset);
            beatMap.m_baseMapMetadata.track_count =
                readMMMU32(base, "track_count", 4);
            beatMap.m_baseMapMetadata.preference_bpm =
                readMMMDouble(base, "bpm", 120.0);
            beatMap.m_baseMapMetadata.map_length =
                readMMMDouble(base, "duration", 0.0);
        }

        auto metadataExtraIt = metadata.find("extra");
        if ( metadataExtraIt != metadata.end() &&
             metadataExtraIt->is_array() ) {
            for ( const auto& extraItem : *metadataExtraIt ) {
                if ( !extraItem.is_object() ) continue;
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
                    copyStringMetadataProperties(props, it.value());
                }
            }
        }
    }

    // 2. Timing 事件。
    auto timingIt = root.find("timing");
    if ( timingIt != root.end() && timingIt->is_array() ) {
        for ( const auto& tJson : *timingIt ) {
            if ( !tJson.is_object() ) continue;
            Timing t;
            t.m_timestamp   = readMMMDouble(tJson, "timestamp", 0.0);
            t.m_bpm         = readMMMDouble(tJson, "bpm", 120.0);
            t.m_beat_length = readMMMDouble(tJson, "beat_length", 500.0);
            t.m_timingEffect =
                timingEffectFromString(readMMMString(tJson, "effect", "bpm"));
            t.m_timingEffectParameter = readMMMDouble(tJson, "param", 0.0);

            auto timingExtraIt = tJson.find("extra");
            if ( timingExtraIt != tJson.end() && timingExtraIt->is_array() ) {
                for ( const auto& extraItem : *timingExtraIt ) {
                    if ( !extraItem.is_object() ) continue;
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
                        copyStringMetadataProperties(props, it.value());
                    }
                }
            }
            beatMap.m_timings.push_back(t);
        }
    }

    // 3. 音符。
    auto noteIt = root.find("note");
    if ( noteIt != root.end() && noteIt->is_array() ) {
        // 预扫描所有折线子物件引用，用于去重（防御旧版本残留的重复数据）
        std::set<std::pair<double, uint32_t>> subNoteKeys;
        for ( const auto& nJson : *noteIt ) {
            if ( !nJson.is_object() ) continue;
            if ( readMMMString(nJson, "type", "note") == "polyline" ) {
                auto subNotesIt = nJson.find("sub_notes");
                if ( subNotesIt != nJson.end() && subNotesIt->is_array() ) {
                    for ( const auto& snJson : *subNotesIt ) {
                        if ( !snJson.is_object() ) continue;
                        subNoteKeys.insert(
                            { readMMMDouble(snJson, "timestamp", 0.0),
                              readMMMU32(snJson, "track", 0) });
                    }
                }
            }
        }

        for ( const auto& nJson : *noteIt ) {
            if ( !nJson.is_object() ) continue;
            std::string type = readMMMString(nJson, "type", "note");
            if ( type == "polyline" ) {
                Polyline& poly   = beatMap.m_noteData.polylines.emplace_back();
                poly.m_type      = NoteType::POLYLINE;
                poly.m_timestamp = readMMMDouble(nJson, "timestamp", 0.0);
                poly.m_track     = readMMMU32(nJson, "track", 0);

                loadNoteMetadata(poly, nJson);

                struct TempSub {
                    NoteType type;
                    double   timestamp;
                    double   duration;
                    int      track;
                    int      dtrack;
                };
                std::vector<TempSub> tempSubs;

                auto subNotesIt = nJson.find("sub_notes");
                if ( subNotesIt != nJson.end() && subNotesIt->is_array() ) {
                    for ( const auto& snJson : *subNotesIt ) {
                        if ( !snJson.is_object() ) continue;
                        std::string stype =
                            readMMMString(snJson, "type", "note");
                        TempSub sn;
                        sn.timestamp = readMMMDouble(snJson, "timestamp", 0.0);
                        sn.track     = readMMMInt(snJson, "track", 0);
                        sn.duration  = 0.0;
                        sn.dtrack    = 0;

                        if ( stype == "hold" ) {
                            sn.duration =
                                readMMMDouble(snJson, "duration", 0.0);
                            if ( sn.duration < 1e-4 ) continue;
                            sn.type = NoteType::HOLD;
                        } else if ( stype == "flick" ) {
                            sn.type   = NoteType::FLICK;
                            sn.dtrack = readMMMInt(snJson, "dtrack", 0);
                        } else {
                            sn.type = NoteType::NOTE;
                        }
                        tempSubs.push_back(sn);
                    }

                    // 迭代清洗逻辑
                    bool changed = true;
                    while ( changed ) {
                        changed = false;

                        // 1. 过滤零值
                        auto it =
                            std::remove_if(tempSubs.begin(),
                                           tempSubs.end(),
                                           [](const auto& s) {
                                               if ( s.type == NoteType::HOLD )
                                                   return s.duration < 1e-4;
                                               if ( s.type == NoteType::FLICK )
                                                   return s.dtrack == 0;
                                               return false;
                                           });
                        if ( it != tempSubs.end() ) {
                            tempSubs.erase(it, tempSubs.end());
                            changed = true;
                        }

                        // 2. 合并同类
                        if ( tempSubs.size() > 1 ) {
                            for ( size_t i = 0; i < tempSubs.size() - 1; ) {
                                auto& curr = tempSubs[i];
                                auto& next = tempSubs[i + 1];
                                if ( curr.type == next.type ) {
                                    if ( curr.type == NoteType::HOLD ) {
                                        curr.duration += next.duration;
                                        tempSubs.erase(tempSubs.begin() + i +
                                                       1);
                                        changed = true;
                                        continue;
                                    } else if ( curr.type == NoteType::FLICK ) {
                                        curr.dtrack += next.dtrack;
                                        tempSubs.erase(tempSubs.begin() + i +
                                                       1);
                                        changed = true;
                                        continue;
                                    }
                                }
                                i++;
                            }
                        }
                    }
                }

                // 根据清洗后的结果决定最终去向
                if ( tempSubs.empty() ) {
                    // 彻底退化为普通 Note
                    Note& n       = beatMap.m_noteData.notes.emplace_back();
                    n.m_type      = NoteType::NOTE;
                    n.m_timestamp = poly.m_timestamp;
                    n.m_track     = poly.m_track;
                    n.m_metadata  = poly.m_metadata;
                    beatMap.m_noteData.polylines
                        .pop_back();  // 移除预先创建的空壳
                } else if ( tempSubs.size() == 1 ) {
                    // 退化为单一物件
                    const auto& s = tempSubs[0];
                    if ( s.type == NoteType::HOLD ) {
                        Hold& h       = beatMap.m_noteData.holds.emplace_back();
                        h.m_type      = NoteType::HOLD;
                        h.m_timestamp = s.timestamp;
                        h.m_track     = s.track;
                        h.m_duration  = s.duration;
                        h.m_metadata  = poly.m_metadata;
                    } else if ( s.type == NoteType::FLICK ) {
                        Flick& f = beatMap.m_noteData.flicks.emplace_back();
                        f.m_type = NoteType::FLICK;
                        f.m_timestamp = s.timestamp;
                        f.m_track     = s.track;
                        f.m_dtrack    = s.dtrack;
                        f.m_metadata  = poly.m_metadata;
                    } else {
                        Note& n       = beatMap.m_noteData.notes.emplace_back();
                        n.m_type      = NoteType::NOTE;
                        n.m_timestamp = s.timestamp;
                        n.m_track     = s.track;
                        n.m_metadata  = poly.m_metadata;
                    }
                    beatMap.m_noteData.polylines
                        .pop_back();  // 移除预先创建的空壳
                } else {
                    // 依然是有效的 Polyline，填充子物件
                    for ( const auto& s : tempSubs ) {
                        if ( s.type == NoteType::HOLD ) {
                            Hold& h  = beatMap.m_noteData.holds.emplace_back();
                            h.m_type = NoteType::HOLD;
                            h.m_timestamp = s.timestamp;
                            h.m_track     = s.track;
                            h.m_duration  = s.duration;
                            h.m_isSubNote = true;
                            poly.m_subNotes.push_back(h);
                            poly.m_subHolds.push_back(h);
                        } else if ( s.type == NoteType::FLICK ) {
                            Flick& f = beatMap.m_noteData.flicks.emplace_back();
                            f.m_type = NoteType::FLICK;
                            f.m_timestamp = s.timestamp;
                            f.m_track     = s.track;
                            f.m_dtrack    = s.dtrack;
                            f.m_isSubNote = true;
                            poly.m_subNotes.push_back(f);
                            poly.m_subFlicks.push_back(f);
                        }
                    }
                    // 更新 Polyline 的锚点信息为第一个子物件
                    if ( !tempSubs.empty() ) {
                        poly.m_timestamp = tempSubs.front().timestamp;
                        poly.m_track     = tempSubs.front().track;
                    }
                }
            } else if ( type == "hold" ) {
                // 跳过属于折线子物件的独立条目（防御旧版本残留的重复数据）
                if ( subNoteKeys.count({ readMMMDouble(nJson, "timestamp", 0.0),
                                         readMMMU32(nJson, "track", 0) }) ) {
                    continue;
                }
                Hold& h       = beatMap.m_noteData.holds.emplace_back();
                h.m_type      = NoteType::HOLD;
                h.m_timestamp = readMMMDouble(nJson, "timestamp", 0.0);
                h.m_track     = readMMMU32(nJson, "track", 0);
                h.m_duration  = readMMMDouble(nJson, "duration", 0.0);
                loadNoteMetadata(h, nJson);
            } else if ( type == "flick" ) {
                // 跳过属于折线子物件的独立条目（防御旧版本残留的重复数据）
                if ( subNoteKeys.count({ readMMMDouble(nJson, "timestamp", 0.0),
                                         readMMMU32(nJson, "track", 0) }) ) {
                    continue;
                }
                Flick& f      = beatMap.m_noteData.flicks.emplace_back();
                f.m_type      = NoteType::FLICK;
                f.m_timestamp = readMMMDouble(nJson, "timestamp", 0.0);
                f.m_track     = readMMMU32(nJson, "track", 0);
                f.m_dtrack    = readMMMInt(nJson, "dtrack", 0);
                loadNoteMetadata(f, nJson);
            } else {
                // 跳过属于折线子物件的独立条目（防御旧版本残留的重复数据）
                if ( subNoteKeys.count({ readMMMDouble(nJson, "timestamp", 0.0),
                                         readMMMU32(nJson, "track", 0) }) ) {
                    continue;
                }
                Note& n       = beatMap.m_noteData.notes.emplace_back();
                n.m_type      = NoteType::NOTE;
                n.m_timestamp = readMMMDouble(nJson, "timestamp", 0.0);
                n.m_track     = readMMMU32(nJson, "track", 0);
                loadNoteMetadata(n, nJson);
            }
        }
    }

    beatMap.sync();

    return beatMap;
}

}  // namespace MMM
