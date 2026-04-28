#pragma once
#include "log/colorful-log.h"
#include "mmm/note/Hold.h"
#include "mmm/note/Flick.h"
#include "mmm/beatmap/BeatMap.h"
#include <cmath>

namespace MMM::Test
{

inline bool compareBeatMaps(const MMM::BeatMap& m1, const MMM::BeatMap& m2)
{
    // 1. Metadata
    if ( m1.m_baseMapMetadata.track_count !=
         m2.m_baseMapMetadata.track_count ) {
        XERROR("Track count mismatch: {} vs {}",
               m1.m_baseMapMetadata.track_count,
               m2.m_baseMapMetadata.track_count);
        return false;
    }

    // 2. Timing
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
        XERROR("Total note count mismatch: {} vs {}", m1.m_allNotes.size(), m2.m_allNotes.size());
        return false;
    }
    for ( size_t i = 0; i < m1.m_allNotes.size(); ++i ) {
        const Note& n1 = m1.m_allNotes[i].get();
        const Note& n2 = m2.m_allNotes[i].get();
        if ( std::abs(n1.m_timestamp - n2.m_timestamp) > 1e-3 || n1.m_track != n2.m_track || n1.m_type != n2.m_type ) {
            XERROR("Note mismatch at index {}: t1={}, tr1={}, typ1={} | t2={}, tr2={}, typ2={}", i, n1.m_timestamp, n1.m_track, (int)n1.m_type, n2.m_timestamp, n2.m_track, (int)n2.m_type);
            return false;
        }
        if ( n1.m_type == NoteType::HOLD ) {
            if ( static_cast<const Hold&>(n1).m_duration != static_cast<const Hold&>(n2).m_duration ) {
                XERROR("Hold duration mismatch at index {}: {} vs {}", i, static_cast<const Hold&>(n1).m_duration, static_cast<const Hold&>(n2).m_duration);
                return false;
            }
        } else if ( n1.m_type == NoteType::FLICK ) {
            if ( static_cast<const Flick&>(n1).m_dtrack != static_cast<const Flick&>(n2).m_dtrack ) {
                XERROR("Flick dtrack mismatch at index {}: {} vs {}", i, static_cast<const Flick&>(n1).m_dtrack, static_cast<const Flick&>(n2).m_dtrack);
                return false;
            }
        }
    }
    return true;
}

}  // namespace MMM::Test
