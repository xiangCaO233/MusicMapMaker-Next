#include "mmm/beatmap/BeatmapSpeedTransform.h"
#include "mmm/SafeParse.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace MMM
{
namespace
{

/// @brief 计算倍速后的毫秒时间。
/// @param value 原毫秒时间。
/// @param speed 倍速倍率。
/// @return 缩放后的毫秒时间。
double scaledMilliseconds(double value, double speed)
{
    return value / speed;
}

/// @brief 计算倍速后的整数毫秒偏移。
/// @param value 原整数毫秒偏移。
/// @param speed 倍速倍率。
/// @return 按最接近整数、半值远离零舍入并限制在 int64 范围内的结果。
std::int64_t scaledOffsetMilliseconds(std::int64_t value, double speed)
{
    const long double scaled =
        static_cast<long double>(value) / static_cast<long double>(speed);
    const long double rounded = std::round(scaled);
    const long double minimum =
        static_cast<long double>(std::numeric_limits<std::int64_t>::min());
    const long double maximum =
        static_cast<long double>(std::numeric_limits<std::int64_t>::max());
    if ( rounded <= minimum ) {
        return std::numeric_limits<std::int64_t>::min();
    }
    if ( rounded >= maximum ) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(rounded);
}

/// @brief 缩放单个普通物件时间。
/// @param note 需要修改的物件。
/// @param speed 倍速倍率。
void scaleNoteTime(Note& note, double speed)
{
    note.m_timestamp = scaledMilliseconds(note.m_timestamp, speed);
}

/// @brief 缩放单个长条物件时间。
/// @param hold 需要修改的长条。
/// @param speed 倍速倍率。
void scaleHoldTime(Hold& hold, double speed)
{
    scaleNoteTime(hold, speed);
    hold.m_duration = scaledMilliseconds(hold.m_duration, speed);
}

/// @brief 同步缩放 Malody time 节点保留的局部 delay。
/// @param timing 需要修改的时间线事件。
/// @param speed 倍速倍率。
void scaleMalodyTimingDelay(Timing& timing, double speed)
{
    auto source =
        timing.m_metadata.timing_properties.find(TimingMetadataType::MALODY);
    if ( source == timing.m_metadata.timing_properties.end() ) return;

    auto delay = source->second.find("delay");
    if ( delay == source->second.end() ) return;

    const auto parsed = nlohmann::json::parse(
        delay->second.begin(), delay->second.end(), nullptr, false);
    if ( parsed.is_discarded() ) return;

    double delayMs = std::numeric_limits<double>::quiet_NaN();
    if ( parsed.is_number() ) {
        delayMs = parsed.get<double>();
    } else if ( parsed.is_string() ) {
        delayMs =
            Internal::safeStod(parsed.get_ref<const std::string&>(), delayMs);
    }
    if ( !std::isfinite(delayMs) ) return;

    delay->second = nlohmann::json(scaledMilliseconds(delayMs, speed)).dump();
}

/// @brief 缩放单个时间线事件。
/// @param timing 需要修改的时间线事件。
/// @param speed 倍速倍率。
void scaleTiming(Timing& timing, double speed)
{
    timing.m_timestamp = scaledMilliseconds(timing.m_timestamp, speed);
    scaleMalodyTimingDelay(timing, speed);
    if ( timing.m_timingEffect != TimingEffect::BPM ) {
        return;
    }

    double bpm = timing.m_bpm;
    if ( bpm <= 0.0 ) {
        bpm = timing.m_timingEffectParameter;
    }
    if ( bpm <= 0.0 && timing.m_beat_length > 0.0 ) {
        bpm = 60000.0 / timing.m_beat_length;
    }
    if ( bpm <= 0.0 || !std::isfinite(bpm) ) {
        return;
    }

    timing.m_bpm                   = bpm * speed;
    timing.m_timingEffectParameter = timing.m_bpm;
    timing.m_beat_length           = 60000.0 / timing.m_bpm;
}

/// @brief 复制并缩放折线中的子物件。
/// @param target 接收子物件的新谱面。
/// @param targetPolyline 正在重建引用的目标折线。
/// @param sourceSubNote 原折线子物件。
/// @param speed 倍速倍率。
void copyScaledPolylineSubNote(BeatMap& target, Polyline& targetPolyline,
                               const Note& sourceSubNote, double speed)
{
    if ( sourceSubNote.m_type == NoteType::HOLD ) {
        const auto& sourceHold = static_cast<const Hold&>(sourceSubNote);
        Hold        hold       = sourceHold;
        hold.m_isSubNote       = true;
        scaleHoldTime(hold, speed);
        target.m_noteData.holds.push_back(std::move(hold));
        auto& copied = target.m_noteData.holds.back();
        targetPolyline.m_subNotes.push_back(copied);
        targetPolyline.m_subHolds.push_back(copied);
        return;
    }

    if ( sourceSubNote.m_type == NoteType::FLICK ) {
        const auto& sourceFlick = static_cast<const Flick&>(sourceSubNote);
        Flick       flick       = sourceFlick;
        flick.m_isSubNote       = true;
        scaleNoteTime(flick, speed);
        target.m_noteData.flicks.push_back(std::move(flick));
        auto& copied = target.m_noteData.flicks.back();
        targetPolyline.m_subNotes.push_back(copied);
        targetPolyline.m_subFlicks.push_back(copied);
        return;
    }

    Note note        = sourceSubNote;
    note.m_isSubNote = true;
    scaleNoteTime(note, speed);
    target.m_noteData.notes.push_back(std::move(note));
    auto& copied = target.m_noteData.notes.back();
    targetPolyline.m_subNotes.push_back(copied);
}

/// @brief 复制并缩放谱面物件。
/// @param target 接收结果的新谱面。
/// @param source 原谱面。
/// @param speed 倍速倍率。
void copyScaledNotes(BeatMap& target, const BeatMap& source, double speed)
{
    target.m_noteData.notes.clear();
    target.m_noteData.holds.clear();
    target.m_noteData.flicks.clear();
    target.m_noteData.polylines.clear();

    for ( const auto& sourceNote : source.m_noteData.notes ) {
        if ( sourceNote.m_isSubNote ) continue;
        Note note = sourceNote;
        scaleNoteTime(note, speed);
        target.m_noteData.notes.push_back(std::move(note));
    }
    for ( const auto& sourceHold : source.m_noteData.holds ) {
        if ( sourceHold.m_isSubNote ) continue;
        Hold hold = sourceHold;
        scaleHoldTime(hold, speed);
        target.m_noteData.holds.push_back(std::move(hold));
    }
    for ( const auto& sourceFlick : source.m_noteData.flicks ) {
        if ( sourceFlick.m_isSubNote ) continue;
        Flick flick = sourceFlick;
        scaleNoteTime(flick, speed);
        target.m_noteData.flicks.push_back(std::move(flick));
    }

    for ( const auto& sourcePolyline : source.m_noteData.polylines ) {
        Polyline polyline = sourcePolyline;
        polyline.m_subNotes.clear();
        polyline.m_subHolds.clear();
        polyline.m_subFlicks.clear();
        scaleNoteTime(polyline, speed);

        for ( const auto& sourceSubNoteRef : sourcePolyline.m_subNotes ) {
            copyScaledPolylineSubNote(
                target, polyline, sourceSubNoteRef.get(), speed);
        }

        if ( !polyline.m_subNotes.empty() ) {
            const auto& firstSub = polyline.m_subNotes.front().get();
            polyline.m_timestamp = firstSub.m_timestamp;
            polyline.m_track     = firstSub.m_track;
        }
        target.m_noteData.polylines.push_back(std::move(polyline));
    }

    target.sync();
}

/// @brief 复制并缩放时间线事件。
/// @param target 接收结果的新谱面。
/// @param source 原谱面。
/// @param speed 倍速倍率。
void copyScaledTimings(BeatMap& target, const BeatMap& source, double speed)
{
    target.m_timings = source.m_timings;
    for ( auto& timing : target.m_timings ) {
        scaleTiming(timing, speed);
    }
}

/// @brief 复制并缩放自动采样时间线。
/// @param target 接收结果的新谱面。
/// @param source 原谱面。
/// @param speed 倍速倍率。
void copyScaledAudioSamples(BeatMap& target, const BeatMap& source,
                            double speed)
{
    target.m_audioSamples = source.m_audioSamples;
    for ( auto& sample : target.m_audioSamples ) {
        sample.m_timestamp = scaledMilliseconds(sample.m_timestamp, speed);
        sample.m_offsetMs  = scaledOffsetMilliseconds(sample.m_offsetMs, speed);
    }
}

}  // namespace

double BeatmapSpeedTransform::calculateContentEndTime(const BeatMap& beatmap)
{
    double endTime = 0.0;
    for ( const auto& timing : beatmap.m_timings ) {
        if ( std::isfinite(timing.m_timestamp) ) {
            endTime = std::max(endTime, timing.m_timestamp);
        }
    }
    for ( const auto& sample : beatmap.m_audioSamples ) {
        const double effectiveTimestamp = sample.effectiveTimestamp();
        if ( std::isfinite(effectiveTimestamp) ) {
            endTime = std::max(endTime, effectiveTimestamp);
        }
    }
    for ( const auto& note : beatmap.m_noteData.notes ) {
        if ( std::isfinite(note.m_timestamp) ) {
            endTime = std::max(endTime, note.m_timestamp);
        }
    }
    for ( const auto& flick : beatmap.m_noteData.flicks ) {
        if ( std::isfinite(flick.m_timestamp) ) {
            endTime = std::max(endTime, flick.m_timestamp);
        }
    }
    for ( const auto& hold : beatmap.m_noteData.holds ) {
        if ( std::isfinite(hold.m_timestamp) &&
             std::isfinite(hold.m_duration) ) {
            endTime = std::max(
                endTime, hold.m_timestamp + std::max(0.0, hold.m_duration));
        }
    }
    for ( const auto& polyline : beatmap.m_noteData.polylines ) {
        if ( std::isfinite(polyline.m_timestamp) ) {
            endTime = std::max(endTime, polyline.m_timestamp);
        }
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            const auto& subNote = subNoteRef.get();
            double      subEnd  = subNote.m_timestamp;
            if ( subNote.m_type == NoteType::HOLD ) {
                const auto& subHold = static_cast<const Hold&>(subNote);
                subEnd += std::max(0.0, subHold.m_duration);
            }
            if ( std::isfinite(subEnd) ) {
                endTime = std::max(endTime, subEnd);
            }
        }
    }
    return endTime;
}

BeatmapSpeedTransformResult BeatmapSpeedTransform::createSpeedVersion(
    const BeatMap& source, const BeatmapSpeedTransformOptions& options)
{
    BeatmapSpeedTransformResult result;
    if ( options.speed <= 0.0 || !std::isfinite(options.speed) ) {
        result.errorMessage = "Invalid speed multiplier";
        return result;
    }

    result.beatmap.m_baseMapMetadata = source.m_baseMapMetadata;
    result.beatmap.m_metadata        = source.m_metadata;

    auto& meta = result.beatmap.m_baseMapMetadata;
    if ( !options.name.empty() ) {
        meta.name = options.name;
    }
    if ( !options.version.empty() ) {
        meta.version = options.version;
    }
    meta.map_path = options.mapPath;
    meta.main_audio_path.clear();
    meta.song_file_hint = options.audioPath;
    if ( meta.preference_bpm > 0.0 && std::isfinite(meta.preference_bpm) ) {
        meta.preference_bpm *= options.speed;
    }
    if ( meta.video_starttime != 0 ) {
        meta.video_starttime =
            static_cast<int32_t>(std::llround(scaledMilliseconds(
                static_cast<double>(meta.video_starttime), options.speed)));
    }

    copyScaledTimings(result.beatmap, source, options.speed);
    copyScaledAudioSamples(result.beatmap, source, options.speed);
    copyScaledNotes(result.beatmap, source, options.speed);
    meta.map_length = calculateContentEndTime(result.beatmap);

    result.success = true;
    return result;
}

}  // namespace MMM
