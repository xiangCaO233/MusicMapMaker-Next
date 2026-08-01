#pragma once

#include "mmm/beatmap/MalodyMode.h"

#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "mmm/SafeParse.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/Timing.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace MMM
{

using json = nlohmann::json;

/// @brief 去除 ASCII 空白，用于解析 Malody mode 元数据。
/// @param text 原始字符串视图。
/// @return 去除首尾空白后的视图。
inline std::string_view trimMalodyAsciiWhitespace(std::string_view text)
{
    while ( !text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                              text.front() == '\n' || text.front() == '\r') ) {
        text.remove_prefix(1);
    }
    while ( !text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                              text.back() == '\n' || text.back() == '\r') ) {
        text.remove_suffix(1);
    }
    return text;
}

/// @brief 无异常解析 Malody mode 字符串。
/// @param text mode 元数据文本。
/// @return 成功时返回 mode 整数，否则返回空。
inline std::optional<int> parseMalodyModeValue(std::string_view text)
{
    text = trimMalodyAsciiWhitespace(text);
    if ( text.empty() ) return std::nullopt;

    int  value = 0;
    auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if ( result.ec != std::errc{} || result.ptr != text.data() + text.size() ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 无异常解析 Malody 元数据整数。
/// @param text 元数据文本。
/// @return 成功时返回 64 位整数，否则返回空。
inline std::optional<int64_t> parseMalodyInt64Value(std::string_view text)
{
    text = trimMalodyAsciiWhitespace(text);
    if ( text.empty() ) return std::nullopt;

    int64_t value = 0;
    auto    result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if ( result.ec != std::errc{} || result.ptr != text.data() + text.size() ) {
        return std::nullopt;
    }
    return value;
}

/// @brief 无异常解析 Malody 元数据 JSON。
/// @param text 元数据文本。
/// @return 成功时返回 JSON 值，否则返回空。
inline std::optional<json> parseMalodyJsonValue(std::string_view text)
{
    json parsed = json::parse(text.begin(), text.end(), nullptr, false);
    if ( parsed.is_discarded() ) {
        return std::nullopt;
    }
    return parsed;
}

/// @brief 解析 JSON 元数据，失败时保留原始字符串。
/// @param text 元数据文本。
/// @return JSON 值或原始字符串。
inline json parseMalodyJsonOrString(const std::string& text)
{
    if ( auto parsed = parseMalodyJsonValue(text) ) {
        return *parsed;
    }
    return text;
}

/// @brief 将 Malody beat 数组转换为小数拍号。
/// @param beatArray Malody beat 数组。
/// @return 成功时返回小数拍号，否则返回空。
inline std::optional<double> malodyBeatArrayToDouble(const json& beatArray)
{
    if ( !beatArray.is_array() || beatArray.size() < 3 ||
         !beatArray[0].is_number() || !beatArray[1].is_number() ||
         !beatArray[2].is_number() ) {
        return std::nullopt;
    }

    const double denominator = beatArray[2].get<double>();
    if ( std::abs(denominator) < std::numeric_limits<double>::epsilon() ) {
        return std::nullopt;
    }
    return beatArray[0].get<double>() +
           beatArray[1].get<double>() / denominator;
}

/// @brief 无异常解析并校验 Malody beat 数组。
/// @param text 元数据文本。
/// @return 成功时返回合法 beat 数组，否则返回空。
inline std::optional<json> parseMalodyBeatJsonValue(const std::string& text)
{
    auto parsed = parseMalodyJsonValue(text);
    if ( !parsed || !malodyBeatArrayToDouble(*parsed) ) {
        return std::nullopt;
    }
    return parsed;
}

/// @brief 判断当前导出器是否支持指定 Malody mode。
/// @param mode Malody 模式编号。
/// @return 支持 key(0) 或 slide(7) 时返回 true。
inline bool isSupportedMalodyExportMode(int mode)
{
    return mode == malodyModeValue(MalodyMode::Key) ||
           mode == malodyModeValue(MalodyMode::Slide);
}

/// @brief 保存谱面为 Malody .mc JSON 文件。
/// @warning 低频导出路径：允许完整遍历谱面数据；位置换算必须以当前时间戳为准。
inline bool saveMalodyMap(const BeatMap& beatMap, std::filesystem::path path)
{
    json fileData;

    // 轨道数和默认宽度
    int trackCount = static_cast<int>(beatMap.m_baseMapMetadata.track_count);
    if ( trackCount <= 0 ) trackCount = 4;

    int defaultXW =
        (trackCount == 4)
            ? 64
            : (trackCount == 5
                   ? 51
                   : (trackCount == 6
                          ? 43
                          : static_cast<int>(std::round(256.0 / trackCount))));
    int defaultWW =
        (trackCount == 4)
            ? 60
            : (trackCount == 5 ? 50 : (trackCount == 6 ? 40 : defaultXW));

    /// @brief 将轨道索引转换为 mode 7 的 x 坐标（画布宽度 256）
    auto columnToX = [&](int column) {
        int x_w    = defaultXW;
        int center = 0;
        if ( trackCount == 4 )
            center = 31;
        else if ( trackCount == 5 )
            center = 25;
        else if ( trackCount == 6 )
            center = 21;
        else
            center = x_w / 2;

        return column * x_w + center;
    };

    // 谱面基础元数据
    const std::string malodyVersion = beatMap.m_baseMapMetadata.version.empty()
                                          ? "default"
                                          : beatMap.m_baseMapMetadata.version;
    auto&             meta          = fileData["meta"];
    meta["creator"]                 = beatMap.m_baseMapMetadata.author;
    meta["version"]                 = malodyVersion;
    meta["background"]              = Config::pathToUtf8(
        beatMap.m_baseMapMetadata.main_cover_path.filename());
    meta["cover"] =
        Config::pathToUtf8(beatMap.m_baseMapMetadata.cover_path.filename());
    meta["id"] = 0;

    /// @brief 获取原始模式，优先从元数据恢复。
    int mode = 7;
    if ( auto it =
             beatMap.m_metadata.map_properties.find(MapMetadataType::MALODY);
         it != beatMap.m_metadata.map_properties.end() ) {
        if ( it->second.contains("mode") ) {
            auto parsedMode = parseMalodyModeValue(it->second.at("mode"));
            if ( !parsedMode ) {
                XERROR("Failed to save Malody map: invalid mode metadata '{}'",
                       it->second.at("mode"));
                return false;
            }
            mode = *parsedMode;
        }
    }
    if ( !isSupportedMalodyExportMode(mode) ) {
        XERROR(
            "Failed to save Malody map: unsupported mode {}. Only key(0) "
            "and slide(7) are supported.",
            mode);
        return false;
    }
    const bool saveAsKeyMode   = mode == malodyModeValue(MalodyMode::Key);
    const bool saveAsSlideMode = mode == malodyModeValue(MalodyMode::Slide);
    meta["mode"]               = mode;

    for ( const auto& polyline : beatMap.m_noteData.polylines ) {
        if ( saveAsKeyMode && !polyline.m_subNotes.empty() &&
             polyline.getSampleBinding() ) {
            XERROR(
                "Malody 导出失败：Key 模式会展开 "
                "Polyline，无法保留其根节点采样绑定");
            return false;
        }
        const auto boundSubNote = std::find_if(
            polyline.m_subNotes.begin(),
            polyline.m_subNotes.end(),
            [](const auto& noteRef) {
                return noteRef.get().getSampleBinding().has_value();
            });
        if ( boundSubNote != polyline.m_subNotes.end() ) {
            XERROR(
                "Malody 导出失败：Polyline 子节点的采样绑定无法由 seg "
                "字段无损表达");
            return false;
        }
    }

    auto& song        = meta["song"];
    song["title"]     = beatMap.m_baseMapMetadata.title;
    song["titleorg"]  = beatMap.m_baseMapMetadata.title_unicode;
    song["artist"]    = beatMap.m_baseMapMetadata.artist;
    song["artistorg"] = beatMap.m_baseMapMetadata.artist_unicode;
    const bool hasExplicitSongFileHint =
        !beatMap.m_baseMapMetadata.song_file_hint.empty();
    const std::filesystem::path& songFileHint =
        hasExplicitSongFileHint ? beatMap.m_baseMapMetadata.song_file_hint
                                : beatMap.m_baseMapMetadata.main_audio_path;
    const std::string songFileValue =
        hasExplicitSongFileHint ? Config::pathToUtf8(songFileHint)
                                : Config::pathToUtf8(songFileHint.filename());
    song["file"] = songFileValue;
    song["bpm"]  = beatMap.m_baseMapMetadata.preference_bpm;

    meta["mode_ext"] = json::object();

    if ( auto it =
             beatMap.m_metadata.map_properties.find(MapMetadataType::MALODY);
         it != beatMap.m_metadata.map_properties.end() ) {
        for ( const auto& [key, val] : it->second ) {
            if ( key == "initialDelay" || key == "audioOffset" ) {
                continue;
            }
            if ( key == "mode_ext" ) {
                meta["mode_ext"] = parseMalodyJsonOrString(val);
            } else if ( key == "id" || key == "preview" || key == "mode" ) {
                if ( auto parsedInteger = parseMalodyInt64Value(val) ) {
                    meta[key] = *parsedInteger;
                } else {
                    meta[key] = val;
                }
            } else if ( key == "extra" ) {
                fileData["extra"] = parseMalodyJsonOrString(val);
            } else {
                meta[key] = parseMalodyJsonOrString(val);
            }
        }
    }
    meta["mode"] = mode;
    meta["free"] = saveAsSlideMode ? 1 : 0;
    if ( saveAsKeyMode ) {
        if ( !meta["mode_ext"].is_object() ) {
            meta["mode_ext"] = json::object();
        }
        if ( !meta["mode_ext"].contains("column") ) {
            meta["mode_ext"]["column"] = trackCount;
        }
    }

    /// @brief 从 Timing 的 Malody 元数据读取有限数值。
    /// @param timing 待读取的 Timing。
    /// @param key 元数据字段名。
    /// @return 字段存在且为有限 JSON 数值时返回其值。
    auto getMalodyTimingNumber =
        [](const Timing&    timing,
           std::string_view key) -> std::optional<double> {
        const auto source = timing.m_metadata.timing_properties.find(
            TimingMetadataType::MALODY);
        if ( source == timing.m_metadata.timing_properties.end() ) {
            return std::nullopt;
        }
        const auto value = source->second.find(key);
        if ( value == source->second.end() ) {
            return std::nullopt;
        }
        const auto parsed = parseMalodyJsonValue(value->second);
        if ( !parsed ) {
            return std::nullopt;
        }
        double number = std::numeric_limits<double>::quiet_NaN();
        if ( parsed->is_number() ) {
            number = parsed->get<double>();
        } else if ( parsed->is_string() ) {
            number = Internal::safeStod(parsed->get_ref<const std::string&>(),
                                        number);
        }
        return std::isfinite(number) ? std::optional<double>{ number }
                                     : std::nullopt;
    };

    /// @brief 从 Timing 的 Malody 元数据恢复原始拍号。
    /// @param timing 待读取的 Timing。
    /// @return 元数据含合法 beat 数组时返回绝对拍数。
    auto getMalodyTimingBeat =
        [](const Timing& timing) -> std::optional<double> {
        const auto source = timing.m_metadata.timing_properties.find(
            TimingMetadataType::MALODY);
        if ( source == timing.m_metadata.timing_properties.end() ) {
            return std::nullopt;
        }
        const auto beat = source->second.find("beat");
        if ( beat == source->second.end() ) {
            return std::nullopt;
        }
        const auto beatJson = parseMalodyJsonValue(beat->second);
        return beatJson ? malodyBeatArrayToDouble(*beatJson) : std::nullopt;
    };

    std::vector<const Timing*> bpmTimings;
    bpmTimings.reserve(beatMap.m_timings.size());
    for ( const auto& timing : beatMap.m_timings ) {
        if ( timing.m_timingEffect == TimingEffect::BPM ) {
            bpmTimings.push_back(&timing);
        }
    }
    std::stable_sort(bpmTimings.begin(),
                     bpmTimings.end(),
                     [](const Timing* lhs, const Timing* rhs) {
                         return lhs->m_timestamp < rhs->m_timestamp;
                     });

    /// @brief 按 Malody 的逐 Timing delay 锚点将毫秒时间转换为拍号。
    /// @param time 待转换的绝对时间，单位为毫秒。
    /// @return Malody beat 三元数组。
    auto timeToBeat = [&](double time) {
        double currentBpm = beatMap.m_baseMapMetadata.preference_bpm > 0
                                ? beatMap.m_baseMapMetadata.preference_bpm
                                : 120.0;
        double lastTime   = 0;
        double lastBeat   = 0;

        for ( const Timing* timing : bpmTimings ) {
            const Timing& t = *timing;
            if ( t.m_timestamp > time + 1e-4 ) break;

            if ( const auto originalBeat = getMalodyTimingBeat(t) ) {
                lastBeat = *originalBeat;
            } else {
                const double delayMs =
                    getMalodyTimingNumber(t, "delay").value_or(0.0);
                lastBeat += (t.m_timestamp - delayMs - lastTime) /
                            (60000.0 / currentBpm);
            }
            lastTime = t.m_timestamp;
            if ( t.m_bpm > 0.0 ) {
                currentBpm = t.m_bpm;
            }
        }
        lastBeat += (time - lastTime) / (60000.0 / currentBpm);

        int    integerBeat = static_cast<int>(std::floor(lastBeat + 1e-6));
        double fraction    = lastBeat - integerBeat;

        if ( fraction < 1e-6 ) return json::array({ integerBeat, 0, 1 });
        if ( fraction > 1.0 - 1e-6 )
            return json::array({ integerBeat + 1, 0, 1 });

        // 尝试常见分母拟合，寻找最简约分
        for ( int den : { 1,  2,  3,  4,   6,   8,   12,  16,  24,  32,
                          48, 64, 96, 192, 288, 384, 480, 768, 960, 1920 } ) {
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

    // 计时与效果数据
    json timeArr = json::array();
    for ( const auto& t : beatMap.m_timings ) {
        if ( t.m_timingEffect == TimingEffect::BPM ) {
            json tj;

            bool hasBeat = false;
            if ( auto it = t.m_metadata.timing_properties.find(
                     TimingMetadataType::MALODY);
                 it != t.m_metadata.timing_properties.end() ) {
                if ( it->second.contains("beat") ) {
                    if ( auto beatJson =
                             parseMalodyBeatJsonValue(it->second.at("beat")) ) {
                        tj["beat"] = *beatJson;
                        hasBeat    = true;
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
                        tj[key] = parseMalodyJsonOrString(val);
                    }
                }
            }
            timeArr.push_back(tj);
        }
    }
    if ( !timeArr.empty() ) {
        fileData["time"] = timeArr;
    }

    bool isOsuSource =
        beatMap.m_metadata.map_properties.find(MapMetadataType::OSU) !=
        beatMap.m_metadata.map_properties.end();

    double currentScroll = -1.0;  // 哨兵值，确保首个 BPM 点必定输出

    // 对计时点排序：相同时间戳时红线(BPM)必须在绿线(SCROLL)之前
    // 确保 scroll=1.0 重置在绿线的 scroll=0.01 覆盖之前输出
    std::vector<const Timing*> sortedTimings;
    sortedTimings.reserve(beatMap.m_timings.size());
    for ( const auto& t : beatMap.m_timings ) {
        sortedTimings.push_back(&t);
    }
    std::stable_sort(sortedTimings.begin(),
                     sortedTimings.end(),
                     [](const Timing* a, const Timing* b) {
                         if ( std::abs(a->m_timestamp - b->m_timestamp) > 1e-4 )
                             return a->m_timestamp < b->m_timestamp;
                         // 同一时间：BPM（红线）排在效果之前
                         if ( a->m_timingEffect != b->m_timingEffect ) {
                             return a->m_timingEffect == TimingEffect::BPM;
                         }
                         return false;
                     });

    json effectArr = json::array();
    for ( const Timing* tp : sortedTimings ) {
        const auto& t = *tp;
        if ( t.m_timingEffect == TimingEffect::BPM && isOsuSource ) {
            // OSU 红线隐式将滑条速度重置为 1.0
            // 仅当当前有效 scroll 不等于 1.0 时才需要显式输出
            if ( currentScroll != 1.0 ) {
                json resetEj;

                bool hasBeat = false;
                if ( auto it = t.m_metadata.timing_properties.find(
                         TimingMetadataType::MALODY);
                     it != t.m_metadata.timing_properties.end() ) {
                    if ( it->second.contains("beat") ) {
                        if ( auto beatJson = parseMalodyBeatJsonValue(
                                 it->second.at("beat")) ) {
                            resetEj["beat"] = *beatJson;
                            hasBeat         = true;
                        }
                    }
                }
                if ( !hasBeat ) {
                    resetEj["beat"] = timeToBeat(t.m_timestamp);
                }
                resetEj["scroll"] = 1.0;
                effectArr.push_back(resetEj);
                currentScroll = 1.0;
            }
        }

        if ( t.m_timingEffect == TimingEffect::SCROLL ||
             t.m_timingEffect == TimingEffect::JUMP ||
             t.m_timingEffect == TimingEffect::HS ) {
            json ej;

            bool hasBeat = false;
            if ( auto it = t.m_metadata.timing_properties.find(
                     TimingMetadataType::MALODY);
                 it != t.m_metadata.timing_properties.end() ) {
                if ( it->second.contains("beat") ) {
                    if ( auto beatJson =
                             parseMalodyBeatJsonValue(it->second.at("beat")) ) {
                        ej["beat"] = *beatJson;
                        hasBeat    = true;
                    }
                }
            }
            if ( !hasBeat ) {
                ej["beat"] = timeToBeat(t.m_timestamp);
            }

            if ( t.m_timingEffect == TimingEffect::SCROLL ) {
                ej["scroll"] = t.m_timingEffectParameter;
            } else if ( t.m_timingEffect == TimingEffect::JUMP ) {
                ej["jump"] = t.m_timingEffectParameter;
            } else {
                ej["hs"] = t.m_timingEffectParameter;
            }

            if ( t.m_timingEffect == TimingEffect::SCROLL ) {
                currentScroll = ej["scroll"];
            }

            // 恢复 Malody 特有字段
            if ( auto it = t.m_metadata.timing_properties.find(
                     TimingMetadataType::MALODY);
                 it != t.m_metadata.timing_properties.end() ) {
                for ( const auto& [key, val] : it->second ) {
                    if ( key != "scroll" && key != "jump" && key != "hs" &&
                         key != "effect" ) {
                        ej[key] = parseMalodyJsonOrString(val);
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

    auto serializeToMalody = [&](const Note& note, bool allowBeatMetadata) {
        json nj;

        bool hasBeat = false;
        if ( allowBeatMetadata ) {
            if ( auto it = note.m_metadata.note_properties.find(
                     NoteMetadataType::MALODY);
                 it != note.m_metadata.note_properties.end() ) {
                if ( it->second.contains("beat") ) {
                    if ( auto beatJson =
                             parseMalodyBeatJsonValue(it->second.at("beat")) ) {
                        nj["beat"] = *beatJson;
                        hasBeat    = true;
                    }
                }
            }
        }

        if ( !hasBeat ) {
            nj["beat"] = timeToBeat(note.m_timestamp);
        }

        if ( saveAsSlideMode ) {
            nj["x"] = columnToX((int)note.m_track);
            // Polyline 和 Hold 根节点使用网格宽度 (64/51/43)，其他使用视觉宽度
            // (60/50/40)
            nj["w"] = (note.m_type == NoteType::POLYLINE ||
                       note.m_type == NoteType::HOLD)
                          ? defaultXW
                          : defaultWW;
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

            if ( saveAsSlideMode ) {
                // 普通 Hold 写成单 seg 模式，且 seg 内不包含 w 和 x
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

            if ( saveAsSlideMode ) {
                // Slide 模式：Flick 导出为 dir + w
                nj["dir"] = (f.m_dtrack < 0) ? 8 : 2;
                int wVal  = defaultWW + std::abs(f.m_dtrack);
                nj["w"]   = wVal;
            }
            // Key 模式下 Flick 不产生额外字段，作为普通 column note 处理
        } else if ( note.m_type == NoteType::POLYLINE && saveAsSlideMode ) {
            const auto& p = static_cast<const Polyline&>(note);

            // 1. 预处理清洗：过滤 0 长度 Hold，合并同向 Flick
            struct CleanSeg {
                NoteType    type;
                double      timestamp;
                double      duration;
                int         track;
                int         dtrack;
                const Note* original_sn;
            };
            std::vector<CleanSeg> cleanSubs;
            for ( const auto& subNoteRef : p.m_subNotes ) {
                const Note& sn = subNoteRef.get();
                if ( sn.m_type == NoteType::HOLD ) {
                    double dur = static_cast<const Hold&>(sn).m_duration;
                    if ( dur < 1e-4 ) continue;
                    cleanSubs.push_back({ NoteType::HOLD,
                                          sn.m_timestamp,
                                          dur,
                                          (int)sn.m_track,
                                          0,
                                          &sn });
                } else if ( sn.m_type == NoteType::FLICK ) {
                    cleanSubs.push_back(
                        { NoteType::FLICK,
                          sn.m_timestamp,
                          0.0,
                          (int)sn.m_track,
                          static_cast<const Flick&>(sn).m_dtrack,
                          &sn });
                }
            }

            // 迭代清洗与合并
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

                // 2. 合并同类（仅限相同时刻）
                if ( cleanSubs.size() > 1 ) {
                    for ( size_t i = 0; i < cleanSubs.size() - 1; ) {
                        auto& curr = cleanSubs[i];
                        auto& next = cleanSubs[i + 1];
                        if ( curr.type == next.type &&
                             std::abs(curr.timestamp + curr.duration -
                                      next.timestamp) < 1e-5 ) {
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

                // 3. 移除冗余 Hold（其后紧跟同时间的 Flick，Hold 偏移为零）
                if ( cleanSubs.size() > 1 ) {
                    for ( size_t i = 0; i < cleanSubs.size() - 1; ) {
                        auto& curr = cleanSubs[i];
                        auto& next = cleanSubs[i + 1];
                        if ( curr.type == NoteType::HOLD &&
                             next.type == NoteType::FLICK &&
                             std::abs((curr.timestamp + curr.duration) -
                                      next.timestamp) < 1e-5 ) {
                            cleanSubs.erase(cleanSubs.begin() + i);
                            changed = true;
                            continue;
                        }
                        i++;
                    }
                }
            }

            // 智能退化检查：如果折线物件实际上只有一个瞬时的滑键段，则导出为标准的
            // dir 模式
            bool exportedAsDir = false;
            if ( cleanSubs.size() == 1 ) {
                const auto& s = cleanSubs[0];
                if ( s.type == NoteType::FLICK &&
                     std::abs(s.timestamp - p.m_timestamp) < 1e-5 ) {
                    nj["dir"]     = (s.dtrack < 0) ? 8 : 2;
                    nj["w"]       = defaultWW + std::abs(s.dtrack);
                    exportedAsDir = true;
                }
            }

            if ( !exportedAsDir && cleanSubs.empty() ) {
                // 所有子物件被清理后无剩余段，降级为普通点物件
                nj.erase("seg");
                nj.erase("w");
            } else if ( !exportedAsDir ) {
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
                        if ( auto it =
                                 s.original_sn->m_metadata.note_properties.find(
                                     NoteMetadataType::MALODY);
                             it !=
                             s.original_sn->m_metadata.note_properties.end() ) {
                            if ( it->second.contains("beat") ) {
                                if ( auto beatJson = parseMalodyBeatJsonValue(
                                         it->second.at("beat")) ) {
                                    sj["beat"]      = *beatJson;
                                    hasBeatMetadata = true;
                                }
                            }
                        }
                    }

                    if ( !hasBeatMetadata ) {
                        sj["beat"] = getRelBeat(current_time, nj["beat"]);
                    }

                    int x_offset = static_cast<int>(
                        std::round((int(current_track) - int(p.m_track)) *
                                   (float)defaultXW));
                    if ( x_offset != 0 ) {
                        sj["x"] = x_offset;
                    }

                    // 从元数据中恢复其他段字段
                    if ( s.original_sn ) {
                        if ( auto it =
                                 s.original_sn->m_metadata.note_properties.find(
                                     NoteMetadataType::MALODY);
                             it !=
                             s.original_sn->m_metadata.note_properties.end() ) {
                            for ( const auto& [key, val] : it->second ) {
                                if ( key != "beat" && key != "x" &&
                                     key != "original_structure" &&
                                     key != "original_structure_flick" ) {
                                    sj[key] = parseMalodyJsonOrString(val);
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
                const bool shouldDropWidth = saveAsKeyMode ||
                                             note.m_type == NoteType::FLICK ||
                                             note.m_type == NoteType::NOTE;
                // 排除已由程序逻辑确定的核心字段，防止旧元数据覆盖新计算结果
                if ( key != "beat" && key != "column" && key != "x" &&
                     key != "endbeat" && key != "seg" && key != "dir" &&
                     key != "type" && key != "sound" && key != "vol" &&
                     key != "original_structure" &&
                     key != "original_structure_flick" &&
                     (!shouldDropWidth || key != "w") ) {
                    nj[key] = parseMalodyJsonOrString(val);
                }
            }
        }
        const auto binding = note.getSampleBinding();
        if ( !binding ) {
            nj.erase("sound");
            nj.erase("vol");
        } else {
            nj["sound"] = binding->m_audioResourceId;
            nj["vol"]   = static_cast<std::int64_t>(
                std::llround(binding->m_volume * 100));
        }
        return nj;
    };

    auto& noteArr = fileData["note"];
    noteArr       = json::array();

    /// @brief 将自动采样对象序列化为 Malody type=1 节点。
    auto serializeAudioSample = [&](const AudioSampleEvent& sample) {
        json sampleJson;

        if ( auto it = sample.m_metadata.sample_properties.find(
                 SampleMetadataType::MALODY);
             it != sample.m_metadata.sample_properties.end() ) {
            for ( const auto& [key, value] : it->second ) {
                if ( key != "beat" && key != "type" && key != "sound" &&
                     key != "offset" && key != "x" && key != "vol" &&
                     key != "original_x" ) {
                    sampleJson[key] = parseMalodyJsonOrString(value);
                }
            }
        }
        // beat 是 m_timestamp 的格式投影；不能让导入时保留的旧 beat
        // 覆盖编辑器已经移动过的锚点。
        sampleJson["beat"] = timeToBeat(sample.m_timestamp);

        sampleJson["type"]   = 1;
        sampleJson["sound"]  = sample.m_audioResourceId;
        sampleJson["offset"] = sample.m_offsetMs;
        sampleJson["x"]      = sample.m_track;
        sampleJson["vol"] =
            static_cast<std::int64_t>(std::llround(sample.m_volume * 100));
        return sampleJson;
    };

    std::vector<const AudioSampleEvent*> sortedSamples;
    sortedSamples.reserve(beatMap.m_audioSamples.size());
    for ( const auto& sample : beatMap.m_audioSamples ) {
        if ( !sample.m_audioResourceId.empty() ) {
            sortedSamples.push_back(&sample);
        }
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
        noteArr.push_back(serializeAudioSample(*sample));
    }

    std::vector<std::pair<const Note*, bool>> sortedNotes;
    for ( const auto& n : beatMap.m_noteData.notes )
        if ( !isSubNote(n) ) sortedNotes.emplace_back(&n, true);
    for ( const auto& n : beatMap.m_noteData.holds )
        if ( !isSubNote(n) ) sortedNotes.emplace_back(&n, true);
    for ( const auto& n : beatMap.m_noteData.flicks )
        if ( !isSubNote(n) ) sortedNotes.emplace_back(&n, true);
    for ( const auto& poly : beatMap.m_noteData.polylines ) {
        if ( isSubNote(poly) ) continue;
        if ( saveAsKeyMode && !poly.m_subNotes.empty() ) {
            for ( const auto& subNoteRef : poly.m_subNotes ) {
                const Note& subNote = subNoteRef.get();
                if ( subNote.m_type == NoteType::HOLD ) {
                    sortedNotes.emplace_back(&subNote, false);
                }
            }
        } else {
            sortedNotes.emplace_back(&poly, true);
        }
    }

    std::sort(
        sortedNotes.begin(),
        sortedNotes.end(),
        [](const auto& a, const auto& b) {
            if ( std::abs(a.first->m_timestamp - b.first->m_timestamp) > 1e-6 )
                return a.first->m_timestamp < b.first->m_timestamp;
            return a.first->m_track < b.first->m_track;
        });

    for ( const auto& [note, allowBeatMetadata] : sortedNotes ) {
        noteArr.push_back(serializeToMalody(*note, allowBeatMetadata));
    }

    std::ofstream ofs(path);
    if ( !ofs.is_open() ) {
        XERROR("Failed to open file [{}] for Malody map write",
               Config::pathToUtf8(path));
        return false;
    }
    ofs << fileData.dump(4);
    XINFO("Successfully saved map to {}", Config::pathToUtf8(path));
    return true;
}

}  // namespace MMM
