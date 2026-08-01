#pragma once
#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/note/Flick.h"
#include "mmm/note/Hold.h"
#include <cmath>

namespace MMM::Test
{

inline bool compareBeatMaps(const MMM::BeatMap& m1, const MMM::BeatMap& m2,
                            bool compareAudioSamples = false)
{
    // 1. 元数据。
    if ( m1.m_baseMapMetadata.track_count !=
         m2.m_baseMapMetadata.track_count ) {
        XERROR("Track count mismatch: {} vs {}",
               m1.m_baseMapMetadata.track_count,
               m2.m_baseMapMetadata.track_count);
        return false;
    }
    if ( compareAudioSamples && m1.m_baseMapMetadata.bgm_track_count !=
                                    m2.m_baseMapMetadata.bgm_track_count ) {
        XERROR("BGM track count mismatch: {} vs {}",
               m1.m_baseMapMetadata.bgm_track_count,
               m2.m_baseMapMetadata.bgm_track_count);
        return false;
    }

    // 2. Timing 事件。
    if ( m1.m_timings.size() != m2.m_timings.size() ) {
        XERROR("Timing count mismatch: {} vs {}",
               m1.m_timings.size(),
               m2.m_timings.size());
        return false;
    }
    for ( size_t i = 0; i < m1.m_timings.size(); ++i ) {
        const auto& t1 = m1.m_timings[i];
        const auto& t2 = m2.m_timings[i];
        if ( std::abs(t1.m_timestamp - t2.m_timestamp) >
                 2.0 ||  // Allow 2ms jitter due to rounding in some formats
             std::abs(t1.m_bpm - t2.m_bpm) > 1e-2 ||
             t1.m_timingEffect != t2.m_timingEffect ) {
            XERROR(
                "Timing mismatch at index {}: t1={}, bpm1={} | t2={}, bpm2={}",
                i,
                t1.m_timestamp,
                t1.m_bpm,
                t2.m_timestamp,
                t2.m_bpm);
            return false;
        }
    }

    if ( m1.m_allNotes.size() != m2.m_allNotes.size() ) {
        XERROR("Total note count mismatch: {} vs {}",
               m1.m_allNotes.size(),
               m2.m_allNotes.size());
        return false;
    }
    for ( size_t i = 0; i < m1.m_allNotes.size(); ++i ) {
        const Note& n1 = m1.m_allNotes[i].get();
        const Note& n2 = m2.m_allNotes[i].get();
        if ( std::abs(n1.m_timestamp - n2.m_timestamp) > 1e-3 ||
             n1.m_track != n2.m_track || n1.m_type != n2.m_type ) {
            XERROR(
                "Note mismatch at index {}: t1={}, tr1={}, typ1={} | t2={}, "
                "tr2={}, typ2={}",
                i,
                n1.m_timestamp,
                n1.m_track,
                (int)n1.m_type,
                n2.m_timestamp,
                n2.m_track,
                (int)n2.m_type);
            return false;
        }
        const auto binding1 = n1.getSampleBinding();
        const auto binding2 = n2.getSampleBinding();
        if ( binding1.has_value() != binding2.has_value() ||
             (binding1 &&
              (binding1->m_audioResourceId != binding2->m_audioResourceId ||
               std::abs(binding1->m_volume - binding2->m_volume) > 1e-6F)) ) {
            XERROR(
                "Note sample binding mismatch at index {}: ref1={}, vol1={} | "
                "ref2={}, vol2={}",
                i,
                binding1 ? binding1->m_audioResourceId : std::string{},
                binding1 ? binding1->m_volume : 0.0F,
                binding2 ? binding2->m_audioResourceId : std::string{},
                binding2 ? binding2->m_volume : 0.0F);
            return false;
        }
        if ( n1.m_type == NoteType::HOLD ) {
            if ( std::abs(static_cast<const Hold&>(n1).m_duration -
                          static_cast<const Hold&>(n2).m_duration) > 0.2 ) {
                XERROR("Hold duration mismatch at index {}: {} vs {}",
                       i,
                       static_cast<const Hold&>(n1).m_duration,
                       static_cast<const Hold&>(n2).m_duration);
                return false;
            }
        } else if ( n1.m_type == NoteType::FLICK ) {
            if ( static_cast<const Flick&>(n1).m_dtrack !=
                 static_cast<const Flick&>(n2).m_dtrack ) {
                XERROR("Flick dtrack mismatch at index {}: {} vs {}",
                       i,
                       static_cast<const Flick&>(n1).m_dtrack,
                       static_cast<const Flick&>(n2).m_dtrack);
                return false;
            }
        }
    }

    // 4. 仅对能够无损表达自动采样对象的格式启用比较。
    if ( !compareAudioSamples ) return true;
    if ( m1.m_audioSamples.size() != m2.m_audioSamples.size() ) {
        XERROR("Audio sample count mismatch: {} vs {}",
               m1.m_audioSamples.size(),
               m2.m_audioSamples.size());
        return false;
    }
    for ( size_t i = 0; i < m1.m_audioSamples.size(); ++i ) {
        const AudioSampleEvent& s1 = m1.m_audioSamples[i];
        const AudioSampleEvent& s2 = m2.m_audioSamples[i];
        if ( std::abs(s1.m_timestamp - s2.m_timestamp) > 1e-3 ||
             s1.m_offsetMs != s2.m_offsetMs ||
             std::abs(s1.effectiveTimestamp() - s2.effectiveTimestamp()) >
                 1e-3 ||
             s1.m_track != s2.m_track ||
             s1.m_audioResourceId != s2.m_audioResourceId ||
             std::abs(s1.m_volume - s2.m_volume) > 1e-6F ) {
            XERROR(
                "Audio sample mismatch at index {}: "
                "t1={}, off1={}, tr1={}, ref1={}, vol1={} | "
                "t2={}, off2={}, tr2={}, ref2={}, vol2={}",
                i,
                s1.m_timestamp,
                s1.m_offsetMs,
                s1.m_track,
                s1.m_audioResourceId,
                s1.m_volume,
                s2.m_timestamp,
                s2.m_offsetMs,
                s2.m_track,
                s2.m_audioResourceId,
                s2.m_volume);
            return false;
        }
    }
    return true;
}

}  // namespace MMM::Test
