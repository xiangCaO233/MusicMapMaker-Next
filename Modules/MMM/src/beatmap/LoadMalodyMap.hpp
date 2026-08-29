#pragma once

#include "MalodyVolume.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/SafeParse.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <vector>

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

/// @brief 无异常读取 Malody JSON 对象中的 64 位整数字段。
/// @param object 待读取的 JSON 对象。
/// @param key 字段名称。
/// @param defaultValue 字段缺失、越界或解析失败时使用的默认值。
/// @return 四舍五入后的 64 位整数或默认值。
inline std::int64_t readMalodyJsonInt64(const json& object, const char* key,
                                        std::int64_t defaultValue)
{
    const long double value = static_cast<long double>(readMalodyJsonDouble(
        object, key, std::numeric_limits<double>::quiet_NaN()));
    if ( !std::isfinite(value) ||
         value < static_cast<long double>(
                     std::numeric_limits<std::int64_t>::min()) ||
         value > static_cast<long double>(
                     std::numeric_limits<std::int64_t>::max()) ) {
        return defaultValue;
    }
    return static_cast<std::int64_t>(std::llround(value));
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
            basemeta.song_file_hint =
                Config::utf8ToPath(song.value("file", ""));
            basemeta.main_audio_path = basemeta.song_file_hint;
            basemeta.preference_bpm  = readMalodyJsonDouble(song, "bpm", 0.0);
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
        double beat = 0.0;
        /// @brief Timing 事件使用的 BPM 值。
        double bpm = -1.0;
        /// @brief 非 BPM 事件使用的效果参数。
        double value = 0.0;
        /// @brief BPM 事件在原始 time 数组中的顺序。
        std::size_t bpmSourceOrder = 0;
        /// @brief 用于往返保存的原始 JSON 对象。
        json raw;
        /// @brief 内部 Timing 效果类型。
        TimingEffect effect{ TimingEffect::BPM };
        /// @brief 是否来自 Malody 的 time 段。
        bool isBpm = false;
    };
    struct BpmEvent {
        /// @brief Malody 拍号位置。
        double beat = 0.0;
        /// @brief 从该拍号开始生效的 BPM 值。
        double bpm = 120.0;
        /// @brief 相对纯 beat 时间追加的局部延迟，单位为毫秒。
        double delayMs = 0.0;
        /// @brief 应用当前 delay 后的绝对时间锚点，单位为毫秒。
        double timestamp = 0.0;
        /// @brief 对应原始 time 数组的稳定顺序。
        std::size_t sourceOrder = 0;
    };
    std::vector<RawEvent> rawEvents;
    std::vector<BpmEvent> bpmEvents;

    if ( fileData.contains("time") ) {
        for ( const auto& t : fileData["time"] ) {
            RawEvent ev;
            ev.beat           = beatToDouble(t.value("beat", json::array()));
            ev.bpm            = readMalodyJsonDouble(t, "bpm", 120.0);
            ev.bpmSourceOrder = bpmEvents.size();
            ev.isBpm          = true;
            ev.raw            = t;

            rawEvents.push_back(ev);
            bpmEvents.push_back(
                { .beat        = ev.beat,
                  .bpm         = ev.bpm,
                  .delayMs     = readMalodyJsonDouble(t, "delay", 0.0),
                  .sourceOrder = ev.bpmSourceOrder });
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
    std::stable_sort(rawEvents.begin(),
                     rawEvents.end(),
                     [](const RawEvent& a, const RawEvent& b) {
                         if ( a.beat != b.beat ) {
                             return a.beat < b.beat;
                         }
                         return a.isBpm && !b.isBpm;
                     });
    std::stable_sort(
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
            if ( n["type"].is_number() ) {
                return std::abs(parseMalodyJsonDouble(n["type"], 0.0) - 1.0) <=
                       std::numeric_limits<double>::epsilon();
            }
        }
        return false;
    };

    /// @brief 判断自动采样是否引用 meta.song.file 指定的主音频。
    /// @param node 待判断的 Malody note 节点。
    /// @return sound 字段与主音频路径或文件名一致时返回 true。
    auto isMainSongSample = [&](const json& node) {
        const auto sound = node.find("sound");
        if ( sound == node.end() || !sound->is_string() ||
             basemeta.song_file_hint.empty() ) {
            return false;
        }
        const std::string& resourceId = sound->get_ref<const std::string&>();
        const std::string  songFileValue =
            Config::pathToUtf8(basemeta.song_file_hint);
        if ( resourceId == songFileValue ) return true;
        return Config::utf8ToPath(resourceId).filename() ==
               basemeta.song_file_hint.filename();
    };

    /// @brief 与首红线对应并锚定歌曲时间零点的主 SOUND 节点。
    const json* wrappedMainSoundNode = nullptr;
    /// @brief 首红线相对主音频零点的规范化相位，单位为毫秒。
    double wrappedFirstTimingPhaseMs = 0.0;
    if ( !bpmEvents.empty() && fileData.contains("note") ) {
        const auto&  firstBpmEvent = bpmEvents.front();
        const double firstBpm =
            firstBpmEvent.bpm > 0.0 ? firstBpmEvent.bpm : 120.0;
        const double firstBeatLengthMs = 60000.0 / firstBpm;
        if ( firstBpmEvent.delayMs >= -1e-6 ) {
            for ( const auto& node : fileData["note"] ) {
                if ( !isSoundNote(node) || !isMainSongSample(node) ||
                     !node.contains("beat") ) {
                    continue;
                }
                const double sampleBeat = beatToDouble(node["beat"]);
                const double sampleOffset =
                    readMalodyJsonDouble(node, "offset", 0.0);
                // 规范文件会按首红线所在半拍选择零 offset 或与 delay
                // 相同的回卷 offset；旧文件也可能始终使用后一种形态。
                // 两种值都只在资源与首 timing 拍号匹配时按主音轨逆变换。
                const bool hasCanonicalMainOffset =
                    std::abs(sampleOffset) <= 0.51;
                const bool hasLegacyPairedOffset =
                    std::abs(sampleOffset - firstBpmEvent.delayMs) <= 0.51;
                if ( std::abs(sampleBeat - firstBpmEvent.beat) <= 1e-6 &&
                     sampleOffset >= -1e-6 &&
                     (hasCanonicalMainOffset || hasLegacyPairedOffset) ) {
                    wrappedMainSoundNode = &node;
                    wrappedFirstTimingPhaseMs =
                        std::fmod(-firstBpmEvent.delayMs, firstBeatLengthMs);
                    if ( wrappedFirstTimingPhaseMs < 0.0 ) {
                        wrappedFirstTimingPhaseMs += firstBeatLengthMs;
                    }
                    if ( std::abs(wrappedFirstTimingPhaseMs) <= 1e-6 ||
                         std::abs(wrappedFirstTimingPhaseMs -
                                  firstBeatLengthMs) <= 1e-6 ) {
                        wrappedFirstTimingPhaseMs = 0.0;
                    }
                    break;
                }
            }
        }
    }

    /// @brief 成对首拍的正相位进位在导入时从所有普通内容拍号中移除。
    const double malodyContentBeatShift =
        wrappedMainSoundNode != nullptr && wrappedFirstTimingPhaseMs > 1e-6
            ? 1.0
            : 0.0;
    if ( malodyContentBeatShift != 0.0 && !bpmEvents.empty() ) {
        const std::size_t wrappedFirstBpmSourceOrder =
            bpmEvents.front().sourceOrder;
        for ( std::size_t index = 1; index < bpmEvents.size(); ++index ) {
            bpmEvents[index].beat -= malodyContentBeatShift;
        }
        for ( auto& event : rawEvents ) {
            const bool isWrappedFirstBpm =
                event.isBpm &&
                event.bpmSourceOrder == wrappedFirstBpmSourceOrder;
            if ( !isWrappedFirstBpm ) {
                event.beat -= malodyContentBeatShift;
            }
        }
        std::stable_sort(rawEvents.begin(),
                         rawEvents.end(),
                         [](const RawEvent& a, const RawEvent& b) {
                             if ( a.beat != b.beat ) {
                                 return a.beat < b.beat;
                             }
                             return a.isBpm && !b.isBpm;
                         });
    }

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

    // 3. 辅助函数：计算拍号锚点的绝对时间 (ms)
    auto getInitialBpm = [&]() {
        double initialBpm =
            basemeta.preference_bpm > 0 ? basemeta.preference_bpm : 120.0;
        if ( !bpmEvents.empty() && bpmEvents.front().beat <= 0.0 ) {
            initialBpm = bpmEvents.front().bpm;
        }
        return initialBpm;
    };
    std::vector<double> bpmTimestampsBySourceOrder(bpmEvents.size(), 0.0);
    double              anchorBeat = 0.0;
    double              anchorTime = 0.0;
    double              anchorBpm  = getInitialBpm();
    for ( std::size_t index = 0; index < bpmEvents.size(); ++index ) {
        auto& ev = bpmEvents[index];
        if ( index == 0 ) {
            const double firstBpm   = ev.bpm > 0.0 ? ev.bpm : getInitialBpm();
            const double beatLength = 60000.0 / firstBpm;
            if ( wrappedMainSoundNode != nullptr ) {
                // 成对编码的首 timing 只保留一拍内相位；beat 上的整拍
                // 前导由后续内容承担，避免首红线停留在数拍之后。
                ev.timestamp = wrappedFirstTimingPhaseMs;
            } else {
                ev.timestamp = ev.beat * beatLength + ev.delayMs;
            }
            bpmTimestampsBySourceOrder[ev.sourceOrder] = ev.timestamp;
            anchorBeat                                 = ev.beat;
            anchorTime                                 = ev.timestamp;
            anchorBpm                                  = ev.bpm;
            continue;
        }
        ev.timestamp = anchorTime +
                       (ev.beat - anchorBeat) * (60000.0 / anchorBpm) +
                       ev.delayMs;
        bpmTimestampsBySourceOrder[ev.sourceOrder] = ev.timestamp;
        anchorBeat                                 = ev.beat;
        anchorTime                                 = ev.timestamp;
        anchorBpm                                  = ev.bpm;
    }

    auto getBpmAtBeat = [&](double beat) {
        double curBpm =
            bpmEvents.empty() ? getInitialBpm() : bpmEvents.front().bpm;
        for ( const auto& ev : bpmEvents ) {
            if ( ev.beat > beat + 1e-9 ) break;
            curBpm = ev.bpm;
        }
        return curBpm;
    };
    auto getAbsTime = [&](double beat) {
        const BpmEvent* relative = nullptr;
        for ( const auto& ev : bpmEvents ) {
            if ( ev.beat > beat + 1e-9 ) break;
            relative = &ev;
        }
        if ( relative == nullptr ) {
            if ( bpmEvents.empty() ) {
                return beat * (60000.0 / getInitialBpm());
            }
            const auto& first = bpmEvents.front();
            return first.timestamp +
                   (beat - first.beat) * (60000.0 / first.bpm);
        }
        return relative->timestamp +
               (beat - relative->beat) * (60000.0 / relative->bpm);
    };

    // 4. 处理时间线点 (Timing Points)
    double currentBpm = getInitialBpm();

    for ( auto& ev : rawEvents ) {
        Timing timing;
        timing.m_timestamp = ev.isBpm
                                 ? bpmTimestampsBySourceOrder[ev.bpmSourceOrder]
                                 : getAbsTime(ev.beat);

        if ( ev.isBpm ) {
            currentBpm                     = ev.bpm;
            timing.m_timingEffect          = TimingEffect::BPM;
            timing.m_bpm                   = currentBpm;
            timing.m_beat_length           = 60000.0 / currentBpm;
            timing.m_timingEffectParameter = currentBpm;
        } else {
            currentBpm                     = getBpmAtBeat(ev.beat);
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

    if ( beatMap.m_timings.empty() ) {
        Timing t;
        t.m_timestamp             = 0.0;
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
    constexpr std::uint32_t     MALODY_LEGACY_SAMPLE_TRACK_BEGIN = 10U;
    std::optional<std::int64_t> legacySampleEffectiveTimestampMs;
    std::uint32_t               nextLegacySampleTrack =
        std::max(MALODY_LEGACY_SAMPLE_TRACK_BEGIN,
                 static_cast<std::uint32_t>(std::max(0, finalK)));
    std::size_t legacyAutoPositionedSampleCount = 0;

    if ( fileData.contains("note") ) {
        for ( const auto& n : fileData["note"] ) {
            if ( !n.contains("beat") ) continue;

            const bool isAutomaticSample = isSoundNote(n);
            const bool isWrappedMainSample =
                isAutomaticSample && &n == wrappedMainSoundNode;
            double startBeat = beatToDouble(n["beat"]);
            if ( !isWrappedMainSample ) {
                startBeat -= malodyContentBeatShift;
            }
            double startTime = getAbsTime(startBeat);

            if ( isAutomaticSample ) {
                const auto soundIt = n.find("sound");
                if ( soundIt == n.end() || !soundIt->is_string() ||
                     soundIt->get_ref<const std::string&>().empty() ) {
                    continue;
                }

                AudioSampleEvent& sample =
                    beatMap.m_audioSamples.emplace_back();
                if ( isWrappedMainSample ) {
                    // 成对字段只描述歌曲相位；MMM 内部将主音频物化在
                    // 时间零点，避免把 Malody 的相位编码误当成局部 offset。
                    sample.m_timestamp = 0.0;
                    sample.m_offsetMs  = 0;
                } else {
                    sample.m_timestamp = startTime;
                    sample.m_offsetMs  = readMalodyJsonInt64(n, "offset", 0);
                }
                sample.m_audioResourceId =
                    soundIt->get_ref<const std::string&>();
                sample.m_volume = Internal::malodyGainPercentToVolume(
                    readMalodyJsonDouble(n, "vol", 0.0));

                auto& props =
                    sample.m_metadata
                        .sample_properties[SampleMetadataType::MALODY];
                for ( auto it = n.begin(); it != n.end(); ++it ) {
                    if ( it.key() != "type" && it.key() != "sound" &&
                         it.key() != "offset" && it.key() != "x" &&
                         it.key() != "vol" ) {
                        props[it.key()] = it.value().dump();
                    }
                }
                const auto   xIt = n.find("x");
                const double parsedX =
                    xIt == n.end()
                        ? std::numeric_limits<double>::quiet_NaN()
                        : parseMalodyJsonDouble(
                              *xIt, std::numeric_limits<double>::quiet_NaN());
                const double roundedX = std::round(parsedX);
                const bool   validBgmTrack =
                    std::isfinite(parsedX) &&
                    std::abs(parsedX - roundedX) <= 1e-6 &&
                    roundedX >= static_cast<double>(finalK) &&
                    roundedX <=
                        static_cast<double>(std::numeric_limits<int>::max());
                if ( xIt == n.end() ) {
                    const std::int64_t effectiveTimestampMs =
                        static_cast<std::int64_t>(
                            std::llround(sample.effectiveTimestamp()));
                    if ( !legacySampleEffectiveTimestampMs.has_value() ||
                         *legacySampleEffectiveTimestampMs !=
                             effectiveTimestampMs ) {
                        legacySampleEffectiveTimestampMs = effectiveTimestampMs;
                        nextLegacySampleTrack            = std::max(
                            MALODY_LEGACY_SAMPLE_TRACK_BEGIN,
                            static_cast<std::uint32_t>(std::max(0, finalK)));
                    }
                    sample.m_track = nextLegacySampleTrack;
                    if ( nextLegacySampleTrack <
                         std::numeric_limits<std::uint32_t>::max() ) {
                        ++nextLegacySampleTrack;
                    }
                    props["original_x"] = "null";
                    ++legacyAutoPositionedSampleCount;
                } else if ( validBgmTrack ) {
                    sample.m_track =
                        static_cast<uint32_t>(static_cast<int>(roundedX));
                } else {
                    sample.m_track      = static_cast<uint32_t>(finalK);
                    props["original_x"] = xIt == n.end() ? "null" : xIt->dump();
                    beatMap.m_loadDiagnostics.push_back(
                        { .m_code = BeatmapLoadDiagnosticCode::
                              AUDIO_SAMPLE_TRACK_RELOCATED,
                          .m_severity = BeatmapLoadDiagnosticSeverity::
                              BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING,
                          .m_message = fmt::format(
                              "Malody 自动采样 '{}' 的轨道 x={} 不属于 "
                              "BGM 区，已迁移到首条 BGM 轨 {}",
                              sample.m_audioResourceId,
                              props["original_x"],
                              finalK),
                          .m_relatedPath = basemeta.map_path });
                    XWARN(
                        "Malody 自动采样 '{}' 的 BGM 轨道 x={} "
                        "非法，已归入首个 "
                        "BGM 轨道 {}",
                        sample.m_audioResourceId,
                        props["original_x"],
                        finalK);
                }

                const std::uint64_t requiredBgmTrackCount64 =
                    static_cast<std::uint64_t>(sample.m_track) -
                    static_cast<std::uint64_t>(finalK) + 1;
                const int requiredBgmTrackCount =
                    static_cast<int>(std::min<std::uint64_t>(
                        requiredBgmTrackCount64,
                        static_cast<std::uint64_t>(
                            std::numeric_limits<int>::max())));
                basemeta.bgm_track_count =
                    std::max(basemeta.bgm_track_count, requiredBgmTrackCount);
                basemeta.map_length =
                    std::max(basemeta.map_length, sample.effectiveTimestamp());
                continue;
            }

            uint32_t track =
                std::clamp(getNoteTrackIndex(n),
                           0u,
                           (uint32_t)std::max(0, basemeta.track_count - 1));

            Note* notePtr = nullptr;

            if ( n.contains("seg") ) {
                auto   segs         = n["seg"];
                double rootBeatRaw  = startBeat;
                double firstSegBeat = rootBeatRaw + beatToDouble(segs[0].value(
                                                        "beat", json::array()));
                double firstTime    = getAbsTime(firstSegBeat);

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
                        double stepTime = getAbsTime(stepBeatValue);

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
                double endBeat =
                    beatToDouble(n["endbeat"]) - malodyContentBeatShift;
                double endTime   = getAbsTime(endBeat);
                Hold&  hold      = beatMap.m_noteData.holds.emplace_back();
                hold.m_type      = NoteType::HOLD;
                hold.m_timestamp = startTime;
                hold.m_track     = track;
                hold.m_duration  = endTime - startTime;
                notePtr          = &hold;
            } else if ( n.contains("dir") ) {
                int trackCount = basemeta.track_count;
                int flickWidthBase =
                    trackCount == 4   ? 60
                    : trackCount == 5 ? 50
                    : trackCount == 6 ? 40
                    : trackCount == 7 ? 30
                    : trackCount == 8
                        ? 20
                        : static_cast<int>(std::round(256.0 / trackCount));

                // Flick 的 w
                // 个位表示跨轨数；十位基数同时落在皮肤的键数识别区间。
                Flick& flick      = beatMap.m_noteData.flicks.emplace_back();
                flick.m_type      = NoteType::FLICK;
                flick.m_timestamp = startTime;
                flick.m_track     = track;

                int wVal = n.value("w", flickWidthBase);
                int distance;
                if ( trackCount == 7 && wVal >= 37 ) {
                    // 兼容旧写出器使用 37 作为 7K Flick 基数的文件。
                    distance = wVal - 37;
                } else if ( trackCount == 8 && wVal >= 32 ) {
                    // 兼容旧写出器使用 32 作为 8K Flick 基数的文件。
                    distance = wVal - 32;
                } else {
                    distance = wVal - flickWidthBase;
                }
                distance = std::max(0, distance);

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
                    notePtr->setSampleBinding(
                        { sound->get_ref<const std::string&>(),
                          Internal::malodyGainPercentToVolume(
                              readMalodyJsonDouble(n, "vol", 0.0)) });
                }

                auto& props = notePtr->m_metadata
                                  .note_properties[NoteMetadataType::MALODY];

                for ( auto it = n.begin(); it != n.end(); ++it ) {
                    if ( it.key() != "beat" && it.key() != "column" &&
                         it.key() != "x" && it.key() != "w" &&
                         it.key() != "type" && it.key() != "dir" &&
                         it.key() != "endbeat" && it.key() != "seg" &&
                         it.key() != "sound" && it.key() != "vol" ) {
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

    if ( legacyAutoPositionedSampleCount > 0 ) {
        const auto legacyTrackBegin =
            std::max(MALODY_LEGACY_SAMPLE_TRACK_BEGIN,
                     static_cast<std::uint32_t>(std::max(0, finalK)));
        beatMap.m_loadDiagnostics.push_back(
            { .m_code = BeatmapLoadDiagnosticCode::AUDIO_SAMPLE_TRACK_RELOCATED,
              .m_severity = BeatmapLoadDiagnosticSeverity::
                  BEATMAP_LOAD_DIAGNOSTIC_SEVERITY_WARNING,
              .m_message = fmt::format(
                  "{} 个缺少 x 的旧版 Malody 自动采样已从绝对轨道 {} "
                  "开始自动分轨；同一实际触发时刻的采样会依次展开",
                  legacyAutoPositionedSampleCount,
                  legacyTrackBegin),
              .m_relatedPath = basemeta.map_path });
        XINFO(
            "已按 Malody Pro Editor 规则为 {} 个缺少 x 的自动采样分轨，"
            "起始绝对轨道为 {}",
            legacyAutoPositionedSampleCount,
            legacyTrackBegin);
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
