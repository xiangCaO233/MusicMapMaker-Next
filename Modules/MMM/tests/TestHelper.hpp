#pragma once
#include "log/colorful-log.h"
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

    // 3. Note count
    // Note: We compare m_noteData counts instead of m_allNotes to be more
    // precise about structural consistency
    size_t count1 = m1.m_noteData.notes.size() + m1.m_noteData.holds.size() +
                    m1.m_noteData.flicks.size() +
                    m1.m_noteData.polylines.size();
    size_t count2 = m2.m_noteData.notes.size() + m2.m_noteData.holds.size() +
                    m2.m_noteData.flicks.size() +
                    m2.m_noteData.polylines.size();

    if ( count1 != count2 ) {
        XERROR(
            "Total structural note count mismatch: {} vs {}", count1, count2);
        XERROR("m1 deques: notes={}, holds={}, flicks={}, polys={}",
               m1.m_noteData.notes.size(),
               m1.m_noteData.holds.size(),
               m1.m_noteData.flicks.size(),
               m1.m_noteData.polylines.size());
        XERROR("m2 deques: notes={}, holds={}, flicks={}, polys={}",
               m2.m_noteData.notes.size(),
               m2.m_noteData.holds.size(),
               m2.m_noteData.flicks.size(),
               m2.m_noteData.polylines.size());
        return false;
    }

    return true;
}

}  // namespace MMM::Test
