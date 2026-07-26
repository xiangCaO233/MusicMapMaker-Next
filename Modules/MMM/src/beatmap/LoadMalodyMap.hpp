#pragma once

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/SafeParse.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>

using json = nlohmann::json;

namespace MMM
{

/// @brief 无异常读取 Malody JSON 数值，兼容数字与数字字符串。
/// @param value 待读取的 JSON 值。
/// @param defaultValue 类型不兼容或解析失败时使用的默认值。
/// @return 有限的解析结果，失败时返回默认值。
inline double parseMalodyJsonDouble(const json& value, double defaultValue)
{
    if ( value.is_number() ) {
        const double number = value.get<double>();
        return std::isfinite(number) ? number : defaultValue;
    }
    if ( value.is_string() ) {
        const double number = Internal::safeStod(
            value.get_ref<const std::string&>(), defaultValue);
        return std::isfinite(number) ? number : defaultValue;
    }
    return defaultValue;
}

/// @brief 无异常读取 Malody JSON 对象中的数值字段。
/// @param object 待读取的 JSON 对象。
/// @param key 字段名称。
/// @param defaultValue 字段缺失或解析失败时使用的默认值。
/// @return 有限的解析结果，失败时返回默认值。
inline double readMalodyJsonDouble(const json& object, const char* key,
                                   double defaultValue)
{
    if ( !object.is_object() ) return defaultValue;
    const auto value = object.find(key);
    if ( value == object.end() ) return defaultValue;
    return parseMalodyJsonDouble(*value, defaultValue);
}

/// @brief 从 Malody `.mc` JSON 文件加载谱面。
/// @param path 待加载的谱面路径。
/// @return 加载后的谱面；文件或 JSON 无效时返回空谱面。
inline BeatMap loadMalodyMap(std::filesystem::path path)
{
    // 创建谱面
    BeatMap beatMap;

    // 获取谱面基本元数据
    BaseMapMeta& basemeta = beatMap.m_baseMapMetadata;
    basemeta.map_path     = path;
    // 切换为绝对路径 (使用 error_code 避免异常)
    std::error_code ec;
    if ( basemeta.map_path.is_relative() ) {
        auto abs_path = std::filesystem::absolute(basemeta.map_path, ec);
        if ( !ec ) {
            basemeta.map_path = abs_path;
        }
    }

    XINFO("加载malody谱面路径:{}", Config::pathToUtf8(basemeta.map_path));

    std::ifstream fs{ path };
    if ( !fs.is_open() ) {
        XERROR("无法打开 malody 谱面文件: {}", Config::pathToUtf8(path));
        return {};
    }

    json        fileData;
    std::string fileContent((std::istreambuf_iterator<char>(fs)),
                            std::istreambuf_iterator<char>());
    fileData = json::parse(fileContent, nullptr, false, true);

    if ( fileData.is_discarded() ) {
        XERROR("解析 malody 谱面 JSON 失败，可能存在严重的编码错误: {}",
               Config::pathToUtf8(path));
        return {};
    }

    // 1. 解析基础元数据 (Meta)
    if ( fileData.contains("meta") ) {
        const auto& meta = fileData["meta"];
        basemeta.author  = meta.value("creator", "");
        basemeta.version = meta.value("version", "");
        basemeta.main_cover_path =
            Config::utf8ToPath(meta.value("background", ""));
        basemeta.cover_path = Config::utf8ToPath(meta.value("cover", ""));

        if ( meta.contains("song") ) {
            const auto& song        = meta["song"];
            basemeta.title          = song.value("title", "");
            basemeta.title_unicode  = song.value("titleorg", "");
            basemeta.artist         = song.value("artist", "");
            basemeta.artist_unicode = song.value("artistorg", "");
            basemeta.main_audio_path =
                Config::utf8ToPath(song.value("file", ""));
            basemeta.preference_bpm = readMalodyJsonDouble(song, "bpm", 0.0);
        }

        if ( meta.contains("mode_ext") ) {
            basemeta.track_count = meta["mode_ext"].value("column", 4);
        }

        // 存储 Malody 特有元数据
        auto& malody_props =
            beatMap.m_metadata.map_properties[MapMetadataType::MALODY];
        malody_props["id"]      = std::to_string(meta.value("id", 0));
        malody_props["preview"] = std::to_string(meta.value("preview", 0));
        const int malodyMode    = meta.value("mode", 0);
        malody_props["mode"]    = std::to_string(malodyMode);
        if ( meta.contains("free") ) {
            const auto& free = meta["free"];
            if ( free.is_number_integer() ) {
                malody_props["free"] = std::to_string(free.get<int>());
            } else if ( free.is_string() ) {
                malody_props["free"] = free.get<std::string>();
            } else {
                malody_props["free"] = free.dump();
            }
        } else if ( malodyMode == 7 ) {
            malody_props["free"] = "1";
        } else if ( malodyMode == 0 ) {
            malody_props["free"] = "0";
        }
        if ( meta.contains("$ver") ) {
            malody_props["$ver"] = std::to_string(meta["$ver"].get<int>());
        }
        if ( meta.contains("aimode") ) {
            malody_props["aimode"] = meta["aimode"].get<std::string>();
        }
        if ( meta.contains("mode_ext") ) {
            malody_props["mode_ext"] = meta["mode_ext"].dump();
        }
    }

    // 存储 extra 顶层扩展数据
    if ( fileData.contains("extra") ) {
        beatMap.m_metadata.map_properties[MapMetadataType::MALODY]["extra"] =
            fileData["extra"].dump();
    }

    // 辅助函数：将 Malody 的 beat [beat_index, numerator, denominator]
    // 转换为绝对拍数 (float)
    auto beatToDouble = [](const json& b) {
        if ( !b.is_array() || b.size() < 3 ) return 0.0;
        // Malody 的 beat 数组约定：第一个元素即为当前拍数索引，后续为细分偏移
        // 绝对拍数 = beat_index + (numerator / denominator)
        const double denominator = parseMalodyJsonDouble(b[2], 1.0);
        if ( std::abs(denominator) <= 1e-9 ) return 0.0;
        return parseMalodyJsonDouble(b[0], 0.0) +
               (parseMalodyJsonDouble(b[1], 0.0) / denominator);
    };

    // 2. 收集原始时间事件
    struct RawEvent {
        /// @brief Malody 拍号位置。
        double beat;
        /// @brief Timing 事件使用的 BPM 值。
        double bpm = -1.0;
        /// @brief 非 BPM 事件使用的效果参数。
        double value = 0.0;
        /// @brief 用于往返保存的原始 JSON 对象。
        json raw;
        /// @brief 内部 Timing 效果类型。
        TimingEffect effect{ TimingEffect::BPM };
        /// @brief 是否来自 Malody 的 time 段。
        bool isBpm = false;
    };
    struct BpmEvent {
        /// @brief Malody 拍号位置。
        double beat;
        /// @brief 从该拍号开始生效的 BPM 值。
        double bpm;
    };
    std::vector<RawEvent> rawEvents;
    std::vector<BpmEvent> bpmEvents;

    double time0Delay = 0.0;
    if ( fileData.contains("time") && fileData["time"].is_array() &&
         !fileData["time"].empty() ) {
        const auto& firstTime = fileData["time"][0];
        time0Delay            = readMalodyJsonDouble(firstTime, "delay", 0.0);
    }
    if ( fileData.contains("time") ) {
        for ( const auto& t : fileData["time"] ) {
            RawEvent ev;
            ev.beat  = beatToDouble(t.value("beat", json::array()));
            ev.bpm   = readMalodyJsonDouble(t, "bpm", 120.0);
            ev.isBpm = true;
            ev.raw   = t;

            rawEvents.push_back(ev);
            bpmEvents.push_back({ ev.beat, ev.bpm });
        }
    }
    if ( fileData.contains("effect") ) {
        for ( const auto& e : fileData["effect"] ) {
            auto pushEffect = [&](const char* key, TimingEffect effect) {
                if ( !e.contains(key) ) return;
                RawEvent ev;
                ev.beat   = beatToDouble(e.value("beat", json::array()));
                ev.value  = readMalodyJsonDouble(e, key, 0.0);
                ev.effect = effect;
                ev.isBpm  = false;
                ev.raw    = e;
                rawEvents.push_back(ev);
            };
            pushEffect("scroll", TimingEffect::SCROLL);
            pushEffect("jump", TimingEffect::JUMP);
            pushEffect("hs", TimingEffect::HS);
        }
    }
    std::sort(rawEvents.begin(),
              rawEvents.end(),
              [](const RawEvent& a, const RawEvent& b) {
                  if ( std::abs(a.beat - b.beat) > 1e-9 )
                      return a.beat < b.beat;
                  return a.isBpm && !b.isBpm;
              });
    std::sort(
        bpmEvents.begin(),
        bpmEvents.end(),
        [](const BpmEvent& a, const BpmEvent& b) { return a.beat < b.beat; });

    // 2.3 获取模式信息
    int malodyMode = 0;
    if ( fileData.contains("meta") ) {
        malodyMode = fileData["meta"].value("mode", 0);
    }

    /// @brief 判断 note 条目是否为音效或 BGM 采样。
    /// type 字段为字符串 ("SOUND") 或旧版整数 (1) 时不参与 key 数推断。
    auto isSoundNote = [](const json& n) -> bool {
        if ( n.contains("type") ) {
            if ( n["type"].is_string() )
                return n["type"].get<std::string>() == "SOUND";
            if ( n["type"].is_number_integer() )
                return n["type"].get<int>() == 1;
        }
        return false;
    };

    // 2.5 预扫描：通过统计学特征自动识别轨道数 (针对 Mode 7 / 坐标模式)
    std::map<int, int> xFreq;
    int                maxColumnField    = -1;
    bool               hasX              = false;
    int                playableNoteCount = 0;
    int                metadataTrackCount =
        basemeta.track_count > 0 ? basemeta.track_count : -1;

    if ( fileData.contains("note") ) {
        for ( const auto& n : fileData["note"] ) {
            if ( isSoundNote(n) ) continue;
            ++playableNoteCount;
            if ( n.contains("column") ) {
                maxColumnField = std::max(maxColumnField, n.value("column", 0));
            } else if ( n.contains("x") ) {
                xFreq[n["x"].get<int>()]++;
                hasX = true;
            }
        }
    }

    // 默认轨道数取 column 字段最大值
    int finalK = std::max(0, maxColumnField + 1);

    // Key 模式的 mode_ext.column 是谱面声明的列数；可玩物件只能在此基础上扩展。
    if ( malodyMode == 0 && metadataTrackCount > 0 ) {
        finalK = std::max(finalK, metadataTrackCount);
    }

    // 如果没有通过 column 识别出轨道数，则回退到元数据中的配置
    if ( finalK <= 0 ) {
        finalK = metadataTrackCount > 0 ? metadataTrackCount : 4;
    }

    // 根据轨道数计算默认间距 (Malody 默认画布宽度为 256)
    float bestW = 256.0f / (float)finalK;
    float bestS = bestW / 2.0f;

    // 根据用户指定的规则调整 4k/5k/其他 的间距和中心点
    if ( finalK == 4 ) {
        bestW = 64.0f;
        bestS = 31.0f;
    } else if ( finalK == 5 ) {
        bestW = 51.0f;
        bestS = 25.0f;
    } else if ( finalK == 6 ) {
        bestW = 43.0f;
        bestS = 21.0f;
    } else {
        bestW = 256.0f / (float)finalK;
        bestS = bestW / 2.0f;
    }

    // 如果存在 x 坐标，通过统计学拟合寻找最匹配的网格系统
    if ( hasX ) {
        double minTotalError = 1e18;
        bool   foundBetter   = false;

        for ( int k : { 4, 5, 6, 7, 8, 9 } ) {
            float w = 256.0f / k;
            float s = w / 2.0f;
            if ( k == 4 ) {
                w = 64.0f;
                s = 31.0f;
            } else if ( k == 5 ) {
                w = 51.0f;
                s = 25.0f;
            } else if ( k == 6 ) {
                w = 43.0f;
                s = 21.0f;
            } else {
                w = 256.0f / k;
                s = w / 2.0f;
            }

            double error = 0;
            for ( auto const& [x, freq] : xFreq ) {
                float target = std::round((x - s) / w) * w + s;
                error += (double)freq * std::abs(x - target);
            }
            if ( error < minTotalError ) {
                minTotalError = error;
                // 只有当拟合误差显著小时才更新轨道参数
                if ( error < (double)playableNoteCount * 5.0 ) {
                    finalK      = k;
                    bestW       = w;
                    bestS       = s;
                    foundBetter = true;
                }
            }
        }
    }

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
    double audioOffset        = 0.0;
    bool   hasSoundNoteOffset = false;
    auto   getInitialBpm      = [&]() {
        double initialBpm =
            basemeta.preference_bpm > 0 ? basemeta.preference_bpm : 120.0;
        if ( !bpmEvents.empty() && bpmEvents.front().beat <= 0.0 ) {
            initialBpm = bpmEvents.front().bpm;
        }
        return initialBpm;
    };
    auto getBpmAtBeat = [&](double beat) {
        double curBpm = getInitialBpm();
        for ( const auto& ev : bpmEvents ) {
            if ( ev.beat > beat + 1e-9 ) break;
            curBpm = ev.bpm;
        }
        return curBpm;
    };
    auto getAbsTime = [&](double beat) {
        double curBpm = getInitialBpm();

        double lastB = 0.0;
        double lastT = 0.0;  // 默认 0.0 对应 beat 0

        for ( const auto& ev : bpmEvents ) {
            if ( ev.beat > beat + 1e-9 ) break;
            if ( ev.beat > lastB ) {
                lastT += (ev.beat - lastB) * (60000.0 / curBpm);
                lastB = ev.beat;
            }
            curBpm = ev.bpm;
        }
        lastT += (beat - lastB) * (60000.0 / curBpm);
        return lastT;
    };

    // 4. 处理音频偏移
    audioOffset = 0.0;
    if ( fileData.contains("note") ) {
        for ( const auto& n : fileData["note"] ) {
            if ( isSoundNote(n) ) {
                std::string soundFile = n.value("sound", "");
                if ( basemeta.main_audio_path.empty() ) {
                    basemeta.main_audio_path = Config::utf8ToPath(soundFile);
                }
                if ( Config::utf8ToPath(soundFile) ==
                         basemeta.main_audio_path ||
                     soundFile.empty() ) {
                    if ( n.contains("offset") ) {
                        audioOffset = readMalodyJsonDouble(n, "offset", 0.0);
                        hasSoundNoteOffset = true;
                    }
                    break;
                }
            }
        }
    }

    if ( !hasSoundNoteOffset ) {
        audioOffset = time0Delay;
    }

    beatMap.m_metadata.map_properties[MapMetadataType::MALODY]["initialDelay"] =
        std::to_string(time0Delay);
    beatMap.m_metadata.map_properties[MapMetadataType::MALODY]["audioOffset"] =
        std::to_string(audioOffset);
    XINFO("找到 Malody 音频偏移: {} ms, Timing Delay: {} ms, 音频文件: {}",
          audioOffset,
          time0Delay,
          Config::pathToUtf8(basemeta.main_audio_path));

    // 4. 处理时间线点 (Timing Points)
    double currentBpm = getInitialBpm();

    /// @brief 统计被钳制到第 0 拍供运行时使用的 Malody 特效事件数量。
    std::size_t clampedNegativeEffectCount = 0;

    for ( auto& ev : rawEvents ) {
        Timing timing;
        double runtimeBeat = ev.beat;
        if ( !ev.isBpm && runtimeBeat < 0.0 ) {
            runtimeBeat = 0.0;
            ++clampedNegativeEffectCount;
        }
        timing.m_timestamp = getAbsTime(runtimeBeat) - audioOffset;

        if ( ev.isBpm ) {
            currentBpm                     = ev.bpm;
            timing.m_timingEffect          = TimingEffect::BPM;
            timing.m_bpm                   = currentBpm;
            timing.m_beat_length           = 60000.0 / currentBpm;
            timing.m_timingEffectParameter = currentBpm;
        } else {
            currentBpm                     = getBpmAtBeat(runtimeBeat);
            timing.m_timingEffect          = ev.effect;
            timing.m_bpm                   = currentBpm;
            timing.m_timingEffectParameter = ev.value;
            timing.m_beat_length           = timing.m_timingEffectParameter;
        }

        auto& malody_timing_props =
            timing.m_metadata.timing_properties[TimingMetadataType::MALODY];
        for ( auto it = ev.raw.begin(); it != ev.raw.end(); ++it ) {
            if ( it.key() != "bpm" && it.key() != "scroll" &&
                 it.key() != "jump" && it.key() != "hs" ) {
                malody_timing_props[it.key()] = it.value().dump();
            }
        }
        if ( !ev.isBpm ) {
            malody_timing_props["effect"] =
                "\"" + timingEffectToString(ev.effect) + "\"";
        }
        if ( beatMap.m_baseMapMetadata.preference_bpm <= 0.0 &&
             timing.m_timingEffect == TimingEffect::BPM ) {
            beatMap.m_baseMapMetadata.preference_bpm = timing.m_bpm;
        }
        beatMap.m_timings.push_back(timing);
    }

    if ( clampedNegativeEffectCount > 0 ) {
        XINFO("已将 {} 个负 beat Malody effect 运行时位置收束到 beat 0",
              clampedNegativeEffectCount);
    }

    if ( beatMap.m_timings.empty() ) {
        Timing t;
        t.m_timestamp             = -audioOffset;
        t.m_bpm                   = currentBpm;
        t.m_beat_length           = 60000.0 / currentBpm;
        t.m_timingEffect          = TimingEffect::BPM;
        t.m_timingEffectParameter = currentBpm;
        beatMap.m_timings.push_back(t);

        if ( basemeta.preference_bpm <= 0.0 ) {
            basemeta.preference_bpm = currentBpm;
        }
    } else {
        if ( basemeta.preference_bpm <= 0.0 ) {
            for ( const auto& t : beatMap.m_timings ) {
                if ( t.m_timingEffect == TimingEffect::BPM ) {
                    basemeta.preference_bpm = t.m_bpm;
                    break;
                }
            }
        }
    }

    // 5. 处理物件 (Notes)
    if ( fileData.contains("note") ) {
        for ( const auto& n : fileData["note"] ) {
            if ( !n.contains("beat") ) continue;
            if ( isSoundNote(n) ) continue;

            double   startBeat = beatToDouble(n["beat"]);
            double   startTime = getAbsTime(startBeat) - audioOffset;
            uint32_t track =
                std::clamp(getNoteTrackIndex(n),
                           0u,
                           (uint32_t)std::max(0, basemeta.track_count - 1));

            Note* notePtr = nullptr;

            if ( n.contains("seg") ) {
                auto   segs         = n["seg"];
                double rootBeatRaw  = beatToDouble(n["beat"]);
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

                if ( segs.size() == 1 ) {
                    if ( firstSegTrack == track ) {
                        Hold& h       = beatMap.m_noteData.holds.emplace_back();
                        h.m_type      = NoteType::HOLD;
                        h.m_timestamp = startTime;
                        h.m_track     = track;
                        h.m_duration  = std::max(0.0, firstTime - startTime);
                        notePtr       = &h;
                    } else if ( firstTime == startTime ) {
                        Flick& f = beatMap.m_noteData.flicks.emplace_back();
                        f.m_type = NoteType::FLICK;
                        f.m_timestamp = startTime;
                        f.m_track     = track;
                        f.m_dtrack    = (int32_t)firstSegTrack - (int32_t)track;
                        notePtr       = &f;
                    }
                }

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

                        if ( stepTime > runningTime + 1e-7 ) {
                            // 长按段。
                            Hold& h  = beatMap.m_noteData.holds.emplace_back();
                            h.m_type = NoteType::HOLD;
                            h.m_timestamp = runningTime;
                            h.m_track     = runningTrack;
                            h.m_duration =
                                std::max(0.0, stepTime - runningTime);
                            h.m_isSubNote = true;

                            poly.m_subNotes.push_back(h);
                            poly.m_subHolds.push_back(h);

                            if ( stepTrack != runningTrack ) {
                                Flick& f =
                                    beatMap.m_noteData.flicks.emplace_back();
                                f.m_type      = NoteType::FLICK;
                                f.m_timestamp = stepTime;
                                f.m_track     = runningTrack;
                                f.m_dtrack =
                                    (int32_t)stepTrack - (int32_t)runningTrack;
                                f.m_isSubNote = true;
                                poly.m_subNotes.push_back(f);
                                poly.m_subFlicks.push_back(f);
                            }
                        } else if ( stepTrack != runningTrack ) {
                            // 瞬时 Flick，仅在轨道发生变化时创建。
                            Flick& f = beatMap.m_noteData.flicks.emplace_back();
                            f.m_type = NoteType::FLICK;
                            f.m_timestamp = runningTime;
                            f.m_track     = runningTrack;
                            f.m_dtrack =
                                (int32_t)stepTrack - (int32_t)runningTrack;
                            f.m_isSubNote = true;
                            poly.m_subNotes.push_back(f);
                            poly.m_subFlicks.push_back(f);
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
                int trackCount = basemeta.track_count;
                int x_w =
                    (trackCount == 4)
                        ? 64
                        : (trackCount == 5
                               ? 51
                               : (trackCount == 6 ? 43
                                                  : static_cast<int>(std::round(
                                                        256.0 / trackCount))));
                int w_w =
                    (trackCount == 4)
                        ? 60
                        : (trackCount == 5 ? 50 : (trackCount == 6 ? 40 : x_w));

                // 处理滑键 Flick (dtrack = (w - w_w) / x_w，方向由 dir 决定)
                Flick& flick      = beatMap.m_noteData.flicks.emplace_back();
                flick.m_type      = NoteType::FLICK;
                flick.m_timestamp = startTime;
                flick.m_track     = track;

                int wVal            = n.value("w", w_w);
                int distance_pixels = wVal - w_w;
                int distance        = distance_pixels;

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
                if ( auto sound = n.find("sound");
                     sound != n.end() && sound->is_string() ) {
                    notePtr->m_boundSound =
                        sound->get_ref<const std::string&>();
                }

                auto& props = notePtr->m_metadata
                                  .note_properties[NoteMetadataType::MALODY];

                for ( auto it = n.begin(); it != n.end(); ++it ) {
                    if ( it.key() != "beat" && it.key() != "column" &&
                         it.key() != "x" && it.key() != "w" &&
                         it.key() != "type" && it.key() != "dir" &&
                         it.key() != "endbeat" && it.key() != "seg" ) {
                        props[it.key()] = it.value().dump();
                    }
                }
                // beatMap.m_allNotes.push_back(*notePtr); // 统一由 sync() 处理

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

    // 最终同步引用
    beatMap.sync();

    XINFO("Successfully loaded Malody map with {} notes and {} timings.",
          beatMap.m_allNotes.size(),
          beatMap.m_timings.size());

    return beatMap;
}

}  // namespace MMM
