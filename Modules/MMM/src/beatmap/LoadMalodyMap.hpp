#pragma once

#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>

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
    struct RawEvent {
        double beat;
        double bpm    = -1.0;
        double scroll = -1.0;
        json   raw;
        bool   isBpm = false;
    };
    std::vector<RawEvent> rawEvents;

    double initialDelay = 0.0;
    if ( fileData.contains("time") ) {
        for ( const auto& t : fileData["time"] ) {
            RawEvent ev;
            ev.beat  = beatToDouble(t.value("beat", json::array()));
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
            RawEvent ev;
            ev.beat   = beatToDouble(e.value("beat", json::array()));
            ev.scroll = e.value("scroll", 1.0);
            ev.isBpm  = false;
            ev.raw    = e;
            rawEvents.push_back(ev);
        }
    }
    std::sort(
        rawEvents.begin(),
        rawEvents.end(),
        [](const RawEvent& a, const RawEvent& b) { return a.beat < b.beat; });

    // 2.3 获取模式信息
    int malodyMode = 0;
    if ( fileData.contains("meta") ) {
        malodyMode = fileData["meta"].value("mode", 0);
    }

    // 2.5 预扫描：通过统计学特征自动识别轨道数 (针对 Mode 7 / 坐标模式)
    std::map<int, int> xFreq;
    int                maxColumnField = -1;
    bool               hasX           = false;

    if ( fileData.contains("note") ) {
        for ( const auto& n : fileData["note"] ) {
            if ( n.contains("column") ) {
                maxColumnField = std::max(maxColumnField, n.value("column", 0));
            } else if ( n.contains("x") ) {
                xFreq[n["x"].get<int>()]++;
                hasX = true;
            }
        }
    }

    // 默认轨道数取 column 字段最大值
    int   finalK = std::max(0, maxColumnField + 1);
    float bestW  = 43.0f;  // 默认为 6k 间距
    float bestS  = 21.0f;

    // 如果没有 column 字段但有 x 坐标，或者处于模式 7 (Mode 7)
    // 通过拟合常见 Mania 布局 (4k-9k) 来寻找最合适的“虚拟轨道”
    if ( hasX && (finalK == 0 || malodyMode == 7) ) {
        double minTotalError = 1e18;
        int    bestK         = 6;
        for ( int k : { 4, 5, 6, 7, 8, 9 } ) {
            float  w     = 256.0f / k;
            float  s     = w / 2.0f;
            double error = 0;
            for ( auto const& [x, freq] : xFreq ) {
                float target = std::round((x - s) / w) * w + s;
                error += (double)freq * std::abs(x - target);
            }
            if ( error < minTotalError ) {
                minTotalError = error;
                bestK         = k;
                bestW         = w;
                bestS         = s;
            }
        }
        finalK = bestK;
    }

    if ( finalK <= 0 ) finalK = 4;  // 最终保底

    // 更新元数据
    basemeta.track_count = finalK;
    XINFO("MC Map track count:{}", basemeta.track_count);

    auto getTrackIndexFromX = [&](int x_val) -> uint32_t {
        if ( finalK <= 0 ) return 0;
        // 计算映射到该虚拟网格的索引
        int idx = static_cast<int>(
            std::round((static_cast<float>(x_val) - bestS) / bestW));
        return static_cast<uint32_t>(std::clamp(idx, 0, finalK - 1));
    };

    auto getNoteTrackIndex = [&](const json& n) -> uint32_t {
        if ( n.contains("column") ) return n.value("column", 0);
        if ( n.contains("x") ) {
            return getTrackIndexFromX(n["x"].get<int>());
        }
        return 0;
    };

    // 3. 辅助函数：计算绝对时间 (ms)
    auto getAbsTime = [&](double beat) {
        double curBpm =
            basemeta.preference_bpm > 0 ? basemeta.preference_bpm : 120.0;
        double lastB = 0.0;
        double lastT = initialDelay;
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

    // 4. 处理音频偏移
    double audioOffset = 0.0;
    if ( fileData.contains("note") ) {
        for ( const auto& n : fileData["note"] ) {
            if ( n.value("type", 0) == 1 ) {
                std::string soundFile = n.value("sound", "");
                if ( basemeta.main_audio_path.empty() ) {
                    basemeta.main_audio_path = soundFile;
                }
                if ( soundFile == basemeta.main_audio_path ||
                     soundFile.empty() ) {
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
        timing.m_timestamp = lastProcessedTime - audioOffset;

        if ( ev.isBpm ) {
            currentBpm                     = ev.bpm;
            timing.m_timingEffect          = TimingEffect::BPM;
            timing.m_bpm                   = currentBpm;
            timing.m_beat_length           = 60000.0 / currentBpm;
            timing.m_timingEffectParameter = currentBpm;
        } else {
            timing.m_timingEffect          = TimingEffect::SCROLL;
            timing.m_bpm                   = currentBpm;
            timing.m_timingEffectParameter = ev.scroll;
            timing.m_beat_length           = ev.scroll;
        }

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
            if ( n.value("type", 0) == 1 ) continue;

            double   startBeat = beatToDouble(n["beat"]);
            double   startTime = getAbsTime(startBeat) - audioOffset;
            uint32_t track =
                std::clamp(getNoteTrackIndex(n),
                           0u,
                           (uint32_t)std::max(0, basemeta.track_count - 1));

            Note* notePtr = nullptr;

            if ( n.contains("seg") ) {
                auto& segs = n["seg"];

                // 1. 获取根节点的绝对拍数 (用于计算子段落的绝对时间)
                double rootBeatRaw = beatToDouble(n["beat"]);

                // 2. 预判定第一个节点的时间和轨道
                double firstSegBeat = rootBeatRaw + beatToDouble(segs[0].value(
                                                        "beat", json::array()));
                double firstTime    = getAbsTime(firstSegBeat) - audioOffset;

                int      rootX   = n.value("x", 0);
                int      xOffset = segs[0].value("x", 0);
                int      firstX  = rootX + xOffset;
                uint32_t firstSegTrack =
                    std::clamp(getTrackIndexFromX(firstX),
                               0u,
                               (uint32_t)std::max(0, basemeta.track_count - 1));

                // 2. 模式 7 优化：如果是单段垂直 Hold 或单段 Flick
                if ( segs.size() == 1 ) {
                    if ( firstTime > startTime && xOffset == 0 &&
                         firstSegTrack == track ) {
                        // 绝对垂直的长条 -> Hold
                        Hold& h       = beatMap.m_noteData.holds.emplace_back();
                        h.m_type      = NoteType::HOLD;
                        h.m_timestamp = startTime;
                        h.m_track     = track;
                        h.m_duration  = firstTime - startTime;
                        notePtr       = &h;
                    } else if ( firstTime == startTime ) {
                        // 瞬发跳变 -> Flick
                        Flick& f = beatMap.m_noteData.flicks.emplace_back();
                        f.m_type = NoteType::FLICK;
                        f.m_timestamp = startTime;
                        f.m_track     = track;
                        f.m_dtrack    = (int32_t)firstSegTrack - (int32_t)track;
                        notePtr       = &f;
                    }
                }

                // 3. 构造 Polyline (针对多段或滑动 Hold)
                if ( !notePtr ) {
                    Polyline& poly =
                        beatMap.m_noteData.polylines.emplace_back();
                    poly.m_type      = NoteType::POLYLINE;
                    poly.m_timestamp = startTime;
                    poly.m_track     = track;

                    uint32_t runningTrack = track;
                    double   runningTime  = startTime;

                    for ( size_t i = 0; i < segs.size(); ++i ) {
                        const auto& s = segs[i];
                        double      stepBeatValue =
                            rootBeatRaw +
                            beatToDouble(s.value("beat", json::array()));
                        double stepTime =
                            getAbsTime(stepBeatValue) - audioOffset;

                        int      stepAbsX  = rootX + s.value("x", 0);
                        uint32_t stepTrack = std::clamp(
                            getTrackIndexFromX(stepAbsX),
                            0u,
                            (uint32_t)std::max(0, basemeta.track_count - 1));

                        if ( stepTime > runningTime ) {
                            // Hold segment
                            Hold& h  = beatMap.m_noteData.holds.emplace_back();
                            h.m_type = NoteType::HOLD;
                            h.m_timestamp = runningTime;
                            h.m_track     = runningTrack;
                            h.m_duration =
                                std::max(0.0, stepTime - runningTime);
                            poly.m_subNotes.push_back(h);
                            poly.m_subHolds.push_back(h);
                            beatMap.m_allNotes.push_back(h);

                            // 如果不是最后一段，或者当前发生了轨道变化，则需要添加
                            // Flick 节点
                            if ( i < segs.size() - 1 ||
                                 stepTrack != runningTrack ) {
                                Flick& f =
                                    beatMap.m_noteData.flicks.emplace_back();
                                f.m_type      = NoteType::FLICK;
                                f.m_timestamp = stepTime;
                                f.m_track     = runningTrack;
                                f.m_dtrack =
                                    (int32_t)stepTrack - (int32_t)runningTrack;
                                poly.m_subNotes.push_back(f);
                                poly.m_subFlicks.push_back(f);
                                beatMap.m_allNotes.push_back(f);
                            }
                        } else {
                            // Instant Flick
                            Flick& f = beatMap.m_noteData.flicks.emplace_back();
                            f.m_type = NoteType::FLICK;
                            f.m_timestamp = runningTime;
                            f.m_track     = runningTrack;
                            f.m_dtrack =
                                (int32_t)stepTrack - (int32_t)runningTrack;
                            poly.m_subNotes.push_back(f);
                            poly.m_subFlicks.push_back(f);
                            beatMap.m_allNotes.push_back(f);
                        }
                        runningTime  = stepTime;
                        runningTrack = stepTrack;
                    }
                    notePtr = &poly;
                }
            } else if ( n.contains("endbeat") ) {
                // 处理长条 Hold
                double endBeat   = beatToDouble(n["endbeat"]);
                double endTime   = getAbsTime(endBeat) - audioOffset;
                Hold&  hold      = beatMap.m_noteData.holds.emplace_back();
                hold.m_type      = NoteType::HOLD;
                hold.m_timestamp = startTime;
                hold.m_track     = track;
                hold.m_duration  = endTime - startTime;
                notePtr          = &hold;
            } else if ( n.contains("dir") ) {
                static std::map<int, int> basewmap = { { 4, 60 },
                                                       { 5, 50 },
                                                       { 6, 40 } };
                // 处理滑键 Flick (dtrack = w - basew，方向由 dir 决定)
                Flick& flick      = beatMap.m_noteData.flicks.emplace_back();
                flick.m_type      = NoteType::FLICK;
                flick.m_timestamp = startTime;
                flick.m_track     = track;
                int distance  = n.value("w", basewmap[basemeta.track_count]) -
                                basewmap[basemeta.track_count];
                int direction = n.value("dir", 0);
                // 8 为左 (-)，2 为右 (+)
                flick.m_dtrack = (direction == 8) ? -distance : distance;
                notePtr        = &flick;
            } else {
                // 普通点点击 Note
                Note& note       = beatMap.m_noteData.notes.emplace_back();
                note.m_type      = NoteType::NOTE;
                note.m_timestamp = startTime;
                note.m_track     = track;
                notePtr          = &note;
            }

            // 物件元数据存储
            if ( notePtr ) {
                auto& malody_note_props =
                    notePtr->m_metadata
                        .note_properties[NoteMetadataType::MALODY];
                for ( auto it = n.begin(); it != n.end(); ++it ) {
                    if ( it.key() != "beat" && it.key() != "endbeat" &&
                         it.key() != "column" && it.key() != "seg" ) {
                        malody_note_props[it.key()] = it.value().dump();
                    }
                }
                beatMap.m_allNotes.push_back(*notePtr);

                // 更新谱面最大长度
                double noteEnd = notePtr->m_timestamp;
                if ( notePtr->m_type == NoteType::HOLD ) {
                    noteEnd += static_cast<Hold*>(notePtr)->m_duration;
                } else if ( notePtr->m_type == NoteType::POLYLINE ) {
                    Polyline& p = *static_cast<Polyline*>(notePtr);
                    if ( !p.m_subNotes.empty() ) {
                        Note& finalSub = p.m_subNotes.back();
                        noteEnd        = finalSub.m_timestamp;
                        if ( finalSub.m_type == NoteType::HOLD ) {
                            noteEnd += static_cast<Hold&>(finalSub).m_duration;
                        }
                    }
                }
                if ( noteEnd > basemeta.map_length ) {
                    basemeta.map_length = noteEnd;
                }
            }
        }
    }

    // 更新谱面元数据
    basemeta.name             = fmt::format("[mc] {} [{}] {}",
                                            basemeta.title,
                                            basemeta.track_count,
                                            basemeta.version);
    beatMap.m_baseMapMetadata = basemeta;

    XINFO("Successfully loaded Malody map with {} notes and {} timings.",
          beatMap.m_allNotes.size(),
          beatMap.m_timings.size());

    XINFO("MC Map track count:{}", basemeta.track_count);

    return beatMap;
}

}  // namespace MMM
