#pragma once

#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace MMM
{

inline BeatMap loadMalodyMap(std::filesystem::path path)
{
    // 创建谱面
    BeatMap beatMap;

    // 获取谱面基本元数据
    BaseMapMeta& basemeta = beatMap.m_baseMapMetadata;
    basemeta.map_path     = path;
    // 切换为绝对路径
    if ( basemeta.map_path.is_relative() ) {
        basemeta.map_path = std::filesystem::absolute(basemeta.map_path);
    }
    XINFO("加载malody谱面路径:{}", basemeta.map_path.string());

    std::ifstream fs{ path };
    if ( !fs.is_open() ) {
        XERROR("无法打开 malody 谱面文件: {}", path.string());
        return {};
    }

    json fileData;
    try {
        fs >> fileData;
    } catch ( const std::exception& e ) {
        XERROR("解析 malody 谱面 JSON 失败: {}", e.what());
        return {};
    }

    // 1. 解析基础元数据 (Meta)
    if ( fileData.contains("meta") ) {
        const auto& meta         = fileData["meta"];
        basemeta.author          = meta.value("creator", "");
        basemeta.version         = meta.value("version", "");
        basemeta.main_cover_path = meta.value("background", "");

        if ( meta.contains("song") ) {
            const auto& song         = meta["song"];
            basemeta.title           = song.value("title", "");
            basemeta.title_unicode   = song.value("titleorg", "");
            basemeta.artist          = song.value("artist", "");
            basemeta.artist_unicode  = song.value("artistorg", "");
            basemeta.main_audio_path = song.value("file", "");
            basemeta.preference_bpm  = song.value("bpm", 0.0);
        }

        if ( meta.contains("mode_ext") ) {
            basemeta.track_count = meta["mode_ext"].value("column", 4);
        }

        // 存储 Malody 特有元数据
        auto& malody_props =
            beatMap.m_metadata.map_properties[MapMetadataType::MALODY];
        malody_props["id"]      = std::to_string(meta.value("id", 0));
        malody_props["preview"] = std::to_string(meta.value("preview", 0));
        malody_props["mode"]    = std::to_string(meta.value("mode", 0));
        if ( meta.contains("aimode") ) {
            malody_props["aimode"] = meta["aimode"].get<std::string>();
        }
        if ( meta.contains("mode_ext") ) {
            malody_props["mode_ext"] = meta["mode_ext"].dump();
        }
    }

    // 辅助函数：将 Malody 的 beat [beat_index, numerator, denominator]
    // 转换为绝对拍数 (float)
    auto beatToDouble = [](const json& b) {
        if ( !b.is_array() || b.size() < 3 ) return 0.0;
        // Malody 的 beat 数组约定：第一个元素即为当前拍数索引，后续为细分偏移
        // 绝对拍数 = beat_index + (numerator / denominator)
        return static_cast<double>(b[0]) +
               (static_cast<double>(b[1]) / static_cast<double>(b[2]));
    };

    // 2. 收集原始时间事件
    struct RawTimingEvent {
        double beat;
        double bpm    = -1.0;
        double scroll = -1.0;
        json   raw;
        bool   isBpm = false;
    };
    std::vector<RawTimingEvent> rawEvents;

    double initialDelay = 0.0;
    if ( fileData.contains("time") ) {
        for ( const auto& t : fileData["time"] ) {
            RawTimingEvent ev;
            ev.beat  = beatToDouble(t["beat"]);
            ev.bpm   = t.value("bpm", 120.0);
            ev.isBpm = true;
            ev.raw   = t;
            if ( t.contains("delay") ) {
                initialDelay = t["delay"];
            }
            rawEvents.push_back(ev);
        }
    }

    if ( fileData.contains("effect") ) {
        for ( const auto& e : fileData["effect"] ) {
            RawTimingEvent ev;
            ev.beat = beatToDouble(e["beat"]);
            if ( e.contains("scroll") ) {
                ev.scroll = e["scroll"];
            }
            ev.isBpm = false;
            ev.raw   = e;
            rawEvents.push_back(ev);
        }
    }

    // 按 beat 排序
    std::sort(
        rawEvents.begin(), rawEvents.end(), [](const auto& a, const auto& b) {
            if ( std::abs(a.beat - b.beat) < 1e-9 ) return a.isBpm && !b.isBpm;
            return a.beat < b.beat;
        });

    // 用于计算任意 beat 的绝对时间
    auto getAbsTime = [&](double beat) {
        double lastB = 0.0;
        double lastT = initialDelay;
        double curBpm =
            basemeta.preference_bpm > 0 ? basemeta.preference_bpm : 120.0;
        if ( !rawEvents.empty() && rawEvents[0].isBpm &&
             rawEvents[0].beat <= 0.0001 ) {
            curBpm = rawEvents[0].bpm;
        }

        for ( const auto& ev : rawEvents ) {
            if ( ev.beat > beat + 1e-9 ) break;
            if ( ev.beat > lastB ) {
                lastT += (ev.beat - lastB) * (60000.0 / curBpm);
                lastB = ev.beat;
            }
            if ( ev.isBpm ) curBpm = ev.bpm;
        }
        lastT += (beat - lastB) * (60000.0 / curBpm);
        return lastT;
    };

    // 3. 处理音频偏移 (Malody 特有：在 note 列表中 type 1 为音频事件)
    double audioOffset = 0.0;
    if ( fileData.contains("note") ) {
        for ( const auto& n : fileData["note"] ) {
            if ( n.value("type", 0) == 1 ) {
                std::string soundFile = n.value("sound", "");
                // 如果元数据里没写音频文件名，以此处找到的第一个音频定义为准
                if ( basemeta.main_audio_path.empty() ) {
                    basemeta.main_audio_path = soundFile;
                }

                // 找到主 BGM 的偏移 (在音轨内部的偏移)
                if ( soundFile == basemeta.main_audio_path || soundFile.empty() ) {
                    audioOffset = n.value("offset", 0.0);
                    XINFO("找到 Malody 音频偏移: {} ms, 音频文件: {}",
                          audioOffset,
                          basemeta.main_audio_path.string());
                    break;
                }
            }
        }
    }

    // 4. 处理时间线点 (Timing Points)
    double currentBpm =
        basemeta.preference_bpm > 0 ? basemeta.preference_bpm : 120.0;
    double lastProcessedBeat = 0.0;
    double lastProcessedTime = initialDelay;

    for ( auto& ev : rawEvents ) {
        double duration =
            (ev.beat - lastProcessedBeat) * (60000.0 / currentBpm);
        lastProcessedTime += duration;
        lastProcessedBeat = ev.beat;

        Timing timing;
        // 应用音频偏移：
        timing.m_timestamp = lastProcessedTime - audioOffset;

        if ( ev.isBpm ) {
            currentBpm                     = ev.bpm;
            timing.m_timingEffect          = TimingEffect::BPM;
            timing.m_bpm                   = currentBpm;
            timing.m_beat_length           = 60000.0 / currentBpm;
            timing.m_timingEffectParameter = currentBpm;
        } else if ( ev.scroll > 0 ) {
            timing.m_timingEffect          = TimingEffect::SCROLL;
            timing.m_bpm                   = currentBpm;
            timing.m_timingEffectParameter = ev.scroll;
            timing.m_beat_length           = ev.scroll;
        } else {
            // 其他特有 Timing，标记为 BPM 并继承当前值
            timing.m_timingEffect          = TimingEffect::BPM;
            timing.m_bpm                   = currentBpm;
            timing.m_beat_length           = 60000.0 / currentBpm;
            timing.m_timingEffectParameter = currentBpm;
        }

        // 存储原始特有字段到元数据
        auto& malody_timing_props =
            timing.m_metadata.timing_properties[TimingMetadataType::MALODY];
        for ( auto it = ev.raw.begin(); it != ev.raw.end(); ++it ) {
            if ( it.key() != "beat" && it.key() != "bpm" &&
                 it.key() != "scroll" && it.key() != "delay" ) {
                malody_timing_props[it.key()] = it.value().dump();
            }
        }
        beatMap.m_timings.push_back(timing);
    }

    // 确保至少有一个初始 Timing
    if ( beatMap.m_timings.empty() ) {
        Timing t;
        t.m_timestamp             = initialDelay - audioOffset;
        t.m_bpm                   = currentBpm;
        t.m_beat_length           = 60000.0 / currentBpm;
        t.m_timingEffect          = TimingEffect::BPM;
        t.m_timingEffectParameter = currentBpm;
        beatMap.m_timings.push_back(t);
    }

    // 5. 处理物件 (Notes)
    if ( fileData.contains("note") ) {
        for ( const auto& n : fileData["note"] ) {
            if ( !n.contains("beat") ) continue;
            if ( n.value("type", 0) == 1 ) continue;  // 跳过音频定义项

            double startBeat = beatToDouble(n["beat"]);
            // 应用音频偏移
            double   startTime = getAbsTime(startBeat) - audioOffset;
            uint32_t track     = n.value("column", 0);

            Note* notePtr = nullptr;
            if ( n.contains("endbeat") ) {
                double endBeat  = beatToDouble(n["endbeat"]);
                double endTime  = getAbsTime(endBeat) - audioOffset;
                Hold&  hold     = beatMap.m_noteData.holds.emplace_back();
                hold.m_type     = NoteType::HOLD;
                hold.m_duration = endTime - startTime;
                notePtr         = &hold;
            } else {
                Note& note  = beatMap.m_noteData.notes.emplace_back();
                note.m_type = NoteType::NOTE;
                notePtr     = &note;
            }

            notePtr->m_timestamp = startTime;
            notePtr->m_track     = track;

            // 物件元数据
            auto& malody_note_props =
                notePtr->m_metadata.note_properties[NoteMetadataType::MALODY];
            for ( auto it = n.begin(); it != n.end(); ++it ) {
                if ( it.key() != "beat" && it.key() != "endbeat" &&
                     it.key() != "column" ) {
                    malody_note_props[it.key()] = it.value().dump();
                }
            }

            beatMap.m_allNotes.push_back(*notePtr);

            // 更新谱面总长度
            double noteEnd =
                startTime + (notePtr->m_type == NoteType::HOLD
                                 ? static_cast<Hold*>(notePtr)->m_duration
                                 : 0.0);
            if ( noteEnd > basemeta.map_length ) {
                basemeta.map_length = noteEnd;
            }
        }
    }

    // 更新谱面名称
    basemeta.name = fmt::format("[mc] {} [{}] {}",
                                basemeta.title,
                                basemeta.track_count,
                                basemeta.version);

    XINFO("Successfully loaded Malody map with {} notes and {} timings.",
          beatMap.m_allNotes.size(),
          beatMap.m_timings.size());

    return beatMap;
}

}  // namespace MMM
