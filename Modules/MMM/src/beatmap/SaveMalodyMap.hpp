#pragma once

#include "MalodyVolume.h"

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

    const double defaultXW     = trackCount == 4   ? 64.0
                                 : trackCount == 5 ? 51.0
                                 : trackCount == 6 ? 43.0
                                 : trackCount == 7
                                     ? 36.5
                                     : 256.0 / static_cast<double>(trackCount);
    const int    defaultWW     = trackCount == 4   ? 60
                                 : trackCount == 5 ? 50
                                 : trackCount == 6 ? 40
                                 : trackCount == 7 ? 30
                                 : trackCount == 8
                                     ? 20
                                     : static_cast<int>(std::round(defaultXW));
    const int defaultLongNoteW = trackCount == 7 || trackCount == 8
                                     ? defaultWW
                                     : static_cast<int>(std::round(defaultXW));
    const int defaultFlickW    = trackCount == 7   ? 30
                                 : trackCount == 8 ? 20
                                                   : defaultWW;

    /// @brief 将轨道索引转换为 mode 7 的 x 坐标（画布宽度 256）
    auto columnToX = [&](int column) {
        double center = 0.0;
        if ( trackCount == 4 )
            center = 31.0;
        else if ( trackCount == 5 )
            center = 25.0;
        else if ( trackCount == 6 )
            center = 21.0;
        else
            center = defaultXW / 2.0;

        return static_cast<int>(
            std::round(static_cast<double>(column) * defaultXW + center));
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
    const std::string songFileNameValue =
        Config::pathToUtf8(Config::utf8ToPath(songFileValue).filename());
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

    /// @brief 判断采样是否对应 Malody 的主音频提示。
    /// @param sample 待判断的自动采样。
    /// @return 资源标识或文件名与 meta.song.file 一致时返回 true。
    auto isMainSongSample = [&](const AudioSampleEvent& sample) {
        if ( songFileValue.empty() ) return false;
        if ( sample.m_audioResourceId == songFileValue ) return true;
        if ( songFileNameValue.empty() ) return false;
        const std::string sampleFileName = Config::pathToUtf8(
            Config::utf8ToPath(sample.m_audioResourceId).filename());
        return sampleFileName == songFileNameValue;
    };

    /// @brief 非 Malody 来源首次投影到 Malody 时使用的拍轴原点。
    const Timing* generatedFirstBpmOrigin = nullptr;
    if ( !bpmTimings.empty() ) {
        const Timing& firstBpm = *bpmTimings.front();
        const auto    source   = firstBpm.m_metadata.timing_properties.find(
            TimingMetadataType::MALODY);
        if ( source == firstBpm.m_metadata.timing_properties.end() ||
             (!source->second.contains("beat") &&
              !source->second.contains("delay")) ) {
            generatedFirstBpmOrigin = &firstBpm;
        }
    }

    /// @brief 与首红线对应且已规范化到时间零点的主音频采样。
    const AudioSampleEvent* wrappedMainSample = nullptr;
    /// @brief 当前配对采样是否来自旧版 MMM 的锚点加整数 offset 形态。
    bool wrappedMainSampleUsesLegacyShape = false;
    /// @brief 音频零点减首红线时间按首拍长回卷后的非负 Malody 值。
    double wrappedMainOffsetMs = 0.0;
    if ( !bpmTimings.empty() ) {
        const Timing& firstBpm = *bpmTimings.front();
        const double  firstBpmValue =
            firstBpm.m_bpm > 0.0 ? firstBpm.m_bpm : 120.0;
        const double firstBeatLength = 60000.0 / firstBpmValue;
        const auto   originalFirstDelay =
            getMalodyTimingNumber(firstBpm, "delay");

        for ( const auto& sample : beatMap.m_audioSamples ) {
            const double effectiveTimestamp = sample.effectiveTimestamp();
            if ( !isMainSongSample(sample) ) {
                continue;
            }

            // 新规范形态必须精确位于时间零点。旧版 Loader 曾保存为
            // “首 Timing 锚点 + 回卷后的整数 offset”，只对该可识别形态
            // 保留 0.51 ms 的整数化兼容窗口，避免吞掉用户的细微移动。
            const bool normalizedShape =
                std::abs(sample.m_timestamp) <= 1e-6 && sample.m_offsetMs == 0;
            bool legacyShape = false;
            if ( !normalizedShape && originalFirstDelay &&
                 *originalFirstDelay > firstBeatLength * 0.5 &&
                 *originalFirstDelay <= firstBeatLength + 1e-6 &&
                 std::abs(sample.m_timestamp - firstBpm.m_timestamp) <= 1e-6 ) {
                double originalTimingPhase =
                    std::fmod(-*originalFirstDelay, firstBeatLength);
                if ( originalTimingPhase < 0.0 ) {
                    originalTimingPhase += firstBeatLength;
                }
                if ( std::abs(originalTimingPhase) <= 1e-6 ||
                     std::abs(originalTimingPhase - firstBeatLength) <= 1e-6 ) {
                    originalTimingPhase = 0.0;
                }
                const double legacyWholeBeat =
                    std::round((firstBpm.m_timestamp - originalTimingPhase) /
                               firstBeatLength);
                const bool legacyEffectiveTimeMatches =
                    std::abs(effectiveTimestamp -
                             legacyWholeBeat * firstBeatLength) <= 0.51;
                const std::int64_t roundedDelay = static_cast<std::int64_t>(
                    std::llround(*originalFirstDelay));
                for ( std::int64_t delta = -1; delta <= 1; ++delta ) {
                    const std::int64_t sourceOffset = roundedDelay + delta;
                    if ( static_cast<double>(sourceOffset) <=
                             firstBeatLength * 0.5 ||
                         std::abs(static_cast<double>(sourceOffset) -
                                  *originalFirstDelay) > 0.51 ) {
                        continue;
                    }
                    const auto legacyOffset = static_cast<std::int64_t>(
                        std::llround(static_cast<double>(sourceOffset) -
                                     firstBeatLength));
                    if ( legacyEffectiveTimeMatches &&
                         sample.m_offsetMs == legacyOffset ) {
                        legacyShape = true;
                        break;
                    }
                }
            }
            if ( !normalizedShape && !legacyShape ) {
                continue;
            }

            const bool currentIsNormalized = wrappedMainSample != nullptr &&
                                             !wrappedMainSampleUsesLegacyShape;
            const bool sameShapeKind = wrappedMainSample != nullptr &&
                                       normalizedShape == currentIsNormalized;
            if ( wrappedMainSample == nullptr ||
                 (normalizedShape && !currentIsNormalized) ||
                 (sameShapeKind &&
                  std::abs(effectiveTimestamp) <
                      std::abs(wrappedMainSample->effectiveTimestamp())) ) {
                wrappedMainSample                = &sample;
                wrappedMainSampleUsesLegacyShape = legacyShape;
            }
        }

        if ( wrappedMainSample != nullptr && firstBeatLength > 0.0 ) {
            const double signedOffset =
                wrappedMainSample->effectiveTimestamp() - firstBpm.m_timestamp;
            wrappedMainOffsetMs = std::fmod(signedOffset, firstBeatLength);
            if ( wrappedMainOffsetMs < 0.0 ) {
                wrappedMainOffsetMs += firstBeatLength;
            }
            if ( std::abs(wrappedMainOffsetMs) <= 1e-6 ||
                 std::abs(wrappedMainOffsetMs - firstBeatLength) <= 1e-6 ) {
                wrappedMainOffsetMs = 0.0;
            }
        }
    }

    /// @brief 首个 BPM 在 Malody 拍轴上的规范化拍号。
    double firstBpmOriginBeat = 0.0;
    /// @brief 首 BPM 的非负回卷 delay。
    double firstBpmDelayMs = 0.0;
    /// @brief 配对主音轨在 Malody 中导出的非负 offset。
    std::int64_t wrappedMainExportOffsetMs = 0;
    /// @brief 首红线归一到首拍后，Malody 内容拍轴的整拍补偿。
    std::int64_t malodyContentBeatShift = 0;
    /// @brief 首红线晚于第一拍时是否需要额外生成首拍锚点。
    bool prependSyntheticFirstBpm = false;
    if ( !bpmTimings.empty() ) {
        const Timing& firstBpm = *bpmTimings.front();
        const double  firstBpmValue =
            firstBpm.m_bpm > 0.0 ? firstBpm.m_bpm : 120.0;
        const double firstBeatLength = 60000.0 / firstBpmValue;

        if ( wrappedMainSample != nullptr ) {
            firstBpmDelayMs = wrappedMainOffsetMs;
            if ( const auto originalDelay =
                     getMalodyTimingNumber(firstBpm, "delay");
                 originalDelay && *originalDelay >= -1e-6 ) {
                double originalPhase =
                    std::fmod(*originalDelay, firstBeatLength);
                if ( originalPhase < 0.0 ) {
                    originalPhase += firstBeatLength;
                }
                const double phaseDifference =
                    std::abs(originalPhase - wrappedMainOffsetMs);
                const double circularPhaseDifference = std::min(
                    phaseDifference, firstBeatLength - phaseDifference);
                const double phaseTolerance =
                    wrappedMainSampleUsesLegacyShape ? 0.51 : 1e-6;
                if ( circularPhaseDifference <= phaseTolerance &&
                     *originalDelay <= firstBeatLength + 1e-6 ) {
                    firstBpmDelayMs = *originalDelay;
                }
            }
            double timingPhase = std::fmod(-firstBpmDelayMs, firstBeatLength);
            if ( timingPhase < 0.0 ) {
                timingPhase += firstBeatLength;
            }
            if ( std::abs(timingPhase) <= 1e-6 ||
                 std::abs(timingPhase - firstBeatLength) <= 1e-6 ) {
                timingPhase = 0.0;
            }
            // Malody 在首红线位于前半拍时需要让主 SOUND 同步携带回卷
            // delay；位于后半拍时则必须保持零 offset，避免游戏端重复
            // 应用相位。两种情况都由其他 time/effect/note[] 对象的拍号
            // 统一承担整拍补偿。
            if ( firstBpmDelayMs > firstBeatLength * 0.5 + 1e-6 ) {
                wrappedMainExportOffsetMs =
                    static_cast<std::int64_t>(std::llround(firstBpmDelayMs));
            }
            firstBpmOriginBeat     = 0.0;
            malodyContentBeatShift = static_cast<std::int64_t>(std::llround(
                (firstBpm.m_timestamp + firstBpmDelayMs) / firstBeatLength));
            prependSyntheticFirstBpm =
                firstBpm.m_timestamp < -1e-6 ||
                firstBpm.m_timestamp >= firstBeatLength - 1e-6;
        } else {
            double wholeBeat =
                std::floor(firstBpm.m_timestamp / firstBeatLength);
            double delayMs = firstBpm.m_timestamp - wholeBeat * firstBeatLength;
            if ( std::abs(delayMs) <= 1e-6 ) {
                delayMs = 0.0;
            } else if ( std::abs(delayMs - firstBeatLength) <= 1e-6 ) {
                wholeBeat += 1.0;
                delayMs = 0.0;
            }
            firstBpmOriginBeat = wholeBeat;
            firstBpmDelayMs    = delayMs;
        }
    }

    /// @brief 按 Malody 的逐 Timing delay 锚点将毫秒时间转换为拍号。
    /// @param time 待转换的绝对时间，单位为毫秒。
    /// @return Malody beat 三元数组。
    auto timeToBeat = [&](double time) {
        double currentBpm = beatMap.m_baseMapMetadata.preference_bpm > 0
                                ? beatMap.m_baseMapMetadata.preference_bpm
                                : 120.0;
        double lastTime   = 0;
        double lastBeat   = 0;

        if ( !bpmTimings.empty() ) {
            const Timing& firstBpm = *bpmTimings.front();
            lastTime               = firstBpm.m_timestamp;
            lastBeat               = firstBpmOriginBeat;
            if ( firstBpm.m_bpm > 0.0 ) {
                currentBpm = firstBpm.m_bpm;
            }
        }

        for ( const Timing* timing : bpmTimings ) {
            const Timing& t = *timing;
            if ( timing == bpmTimings.front() ) continue;
            if ( t.m_timestamp > time + 1e-4 ) break;

            const double delayMs =
                getMalodyTimingNumber(t, "delay").value_or(0.0);
            lastBeat +=
                (t.m_timestamp - delayMs - lastTime) / (60000.0 / currentBpm);
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

    /// @brief 将普通谱面内容转换到带首拍相位补偿的 Malody 拍轴。
    /// @param time 物件绝对时间，单位为毫秒。
    /// @return 已应用整拍补偿的 Malody beat 三元数组。
    auto timeToMalodyContentBeat = [&](double time) {
        json beat = timeToBeat(time);
        if ( malodyContentBeatShift != 0 ) {
            beat[0] = beat[0].get<std::int64_t>() + malodyContentBeatShift;
        }
        return beat;
    };

    // 计时与效果数据
    json timeArr = json::array();
    if ( prependSyntheticFirstBpm && !bpmTimings.empty() ) {
        const Timing& firstBpm = *bpmTimings.front();
        timeArr.push_back({ { "beat", json::array({ 0, 0, 1 }) },
                            { "bpm", firstBpm.m_bpm },
                            { "delay", firstBpmDelayMs } });
    }
    for ( const auto& t : beatMap.m_timings ) {
        if ( t.m_timingEffect == TimingEffect::BPM ) {
            json tj;

            // beat 必须由当前内部时间线重新计算；导入元数据中的旧拍号
            // 只用于诊断，不能覆盖用户移动或变速后的实际位置。
            const bool isFirstBpm =
                !bpmTimings.empty() && &t == bpmTimings.front();
            tj["beat"] = isFirstBpm && !prependSyntheticFirstBpm
                             ? timeToBeat(t.m_timestamp)
                             : timeToMalodyContentBeat(t.m_timestamp);

            tj["bpm"] = t.m_bpm;

            // 恢复 Malody 特有字段
            if ( auto it = t.m_metadata.timing_properties.find(
                     TimingMetadataType::MALODY);
                 it != t.m_metadata.timing_properties.end() ) {
                for ( const auto& [key, val] : it->second ) {
                    if ( key != "bpm" && key != "beat" ) {
                        tj[key] = parseMalodyJsonOrString(val);
                    }
                }
            }
            if ( isFirstBpm && !prependSyntheticFirstBpm ) {
                tj["beat"]  = timeToBeat(t.m_timestamp);
                tj["delay"] = firstBpmDelayMs;
            } else if ( isFirstBpm ) {
                // 额外首拍锚点承载歌曲相位；原首红线保留原时间，但不
                // 重复附加 delay。
                tj.erase("delay");
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

                resetEj["beat"]   = timeToMalodyContentBeat(t.m_timestamp);
                resetEj["scroll"] = 1.0;
                effectArr.push_back(resetEj);
                currentScroll = 1.0;
            }
        }

        if ( t.m_timingEffect == TimingEffect::SCROLL ||
             t.m_timingEffect == TimingEffect::JUMP ||
             t.m_timingEffect == TimingEffect::HS ) {
            json ej;
            ej["beat"] = timeToMalodyContentBeat(t.m_timestamp);

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
                         key != "effect" && key != "beat" ) {
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

    auto serializeToMalody = [&](const Note& note) {
        json nj;
        nj["beat"] = timeToMalodyContentBeat(note.m_timestamp);

        if ( saveAsSlideMode ) {
            nj["x"] = columnToX((int)note.m_track);
            // 4K 至 6K 的 Polyline 和 Hold 根节点沿用网格宽度；7K、8K
            // 按皮肤的十位宽度规则固定为 30、20，避免键数误判。
            nj["w"] = (note.m_type == NoteType::POLYLINE ||
                       note.m_type == NoteType::HOLD)
                          ? defaultLongNoteW
                          : defaultWW;
        } else {
            nj["column"] = (int)note.m_track;
        }

        auto getRelBeat = [&](double targetTime, const json& rootBeatArr) {
            double rootBeatVal =
                rootBeatArr[0].get<double>() +
                (rootBeatArr[1].get<double>() / rootBeatArr[2].get<double>());
            auto   relBeatArr = timeToMalodyContentBeat(targetTime);
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
                nj["endbeat"] =
                    timeToMalodyContentBeat(h.m_timestamp + h.m_duration);
            }
        } else if ( note.m_type == NoteType::FLICK ) {
            const auto& f = static_cast<const Flick&>(note);

            if ( saveAsSlideMode ) {
                // Slide 模式：Flick 导出为 dir + w
                nj["dir"] = (f.m_dtrack < 0) ? 8 : 2;
                int wVal  = defaultFlickW + std::abs(f.m_dtrack);
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
                    nj["w"]       = defaultFlickW + std::abs(s.dtrack);
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

                    double current_time  = s.timestamp;
                    int    current_track = s.track;
                    if ( s.type == NoteType::HOLD ) {
                        current_time += s.duration;
                    } else if ( s.type == NoteType::FLICK ) {
                        current_track += s.dtrack;
                    }

                    json sj;
                    sj["beat"] = getRelBeat(current_time, nj["beat"]);

                    int x_offset = columnToX(current_track) -
                                   columnToX(static_cast<int>(p.m_track));
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
            nj["vol"] = Internal::volumeToMalodyGainPercent(binding->m_volume);
        }
        return nj;
    };

    auto& noteArr = fileData["note"];
    noteArr       = json::array();

    /// @brief 按当前模式序列化 Malody 自动采样对象。
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
        // 覆盖编辑器已经移动过的锚点。位于生成拍轴之前的采样锚定到
        // 首 BPM 的规范化拍号，并把时间差折入自身
        // offset，以保持实际播放时刻不变。
        std::int64_t exportedOffset = sample.m_offsetMs;
        if ( &sample == wrappedMainSample ) {
            sampleJson["beat"] = timeToBeat(bpmTimings.front()->m_timestamp);
            // 主 SOUND 的 offset 必须与首红线所在半拍匹配；前半拍保留
            // 回卷 delay，后半拍使用零 offset。
            exportedOffset = wrappedMainExportOffsetMs;
        } else if ( generatedFirstBpmOrigin != nullptr &&
                    sample.m_timestamp <
                        generatedFirstBpmOrigin->m_timestamp - 1e-4 ) {
            sampleJson["beat"] =
                timeToMalodyContentBeat(generatedFirstBpmOrigin->m_timestamp);
            const long double adjustedOffset =
                static_cast<long double>(sample.m_offsetMs) +
                static_cast<long double>(sample.m_timestamp) -
                static_cast<long double>(generatedFirstBpmOrigin->m_timestamp);
            const long double minimumOffset = static_cast<long double>(
                std::numeric_limits<std::int64_t>::min());
            const long double maximumOffset = static_cast<long double>(
                std::numeric_limits<std::int64_t>::max());
            if ( adjustedOffset <= minimumOffset ) {
                exportedOffset = std::numeric_limits<std::int64_t>::min();
            } else if ( adjustedOffset >= maximumOffset ) {
                exportedOffset = std::numeric_limits<std::int64_t>::max();
            } else {
                exportedOffset =
                    static_cast<std::int64_t>(std::llround(adjustedOffset));
            }
        } else {
            sampleJson["beat"] = timeToMalodyContentBeat(sample.m_timestamp);
        }

        // Malody Slide 游戏逻辑只识别字符串 SOUND；Key 模式保留数值 1，
        // 兼容 BMS 编辑与既有 Key 谱面。
        if ( saveAsSlideMode ) {
            sampleJson["type"] = "SOUND";
        } else {
            sampleJson["type"] = 1;
        }
        sampleJson["sound"]  = sample.m_audioResourceId;
        sampleJson["offset"] = exportedOffset;
        if ( !saveAsSlideMode ) {
            sampleJson["x"] = sample.m_track;
        }
        sampleJson["vol"] =
            Internal::volumeToMalodyGainPercent(sample.m_volume);
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

    std::vector<const Note*> sortedNotes;
    for ( const auto& n : beatMap.m_noteData.notes )
        if ( !isSubNote(n) ) sortedNotes.push_back(&n);
    for ( const auto& n : beatMap.m_noteData.holds )
        if ( !isSubNote(n) ) sortedNotes.push_back(&n);
    for ( const auto& n : beatMap.m_noteData.flicks )
        if ( !isSubNote(n) ) sortedNotes.push_back(&n);
    for ( const auto& poly : beatMap.m_noteData.polylines ) {
        if ( isSubNote(poly) ) continue;
        if ( saveAsKeyMode && !poly.m_subNotes.empty() ) {
            for ( const auto& subNoteRef : poly.m_subNotes ) {
                const Note& subNote = subNoteRef.get();
                if ( subNote.m_type == NoteType::HOLD ) {
                    sortedNotes.push_back(&subNote);
                }
            }
        } else {
            sortedNotes.push_back(&poly);
        }
    }

    std::sort(sortedNotes.begin(),
              sortedNotes.end(),
              [](const auto& a, const auto& b) {
                  if ( std::abs(a->m_timestamp - b->m_timestamp) > 1e-6 )
                      return a->m_timestamp < b->m_timestamp;
                  return a->m_track < b->m_track;
              });

    for ( const Note* note : sortedNotes ) {
        noteArr.push_back(serializeToMalody(*note));
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
