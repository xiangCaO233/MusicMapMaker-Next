#include "log/colorful-log.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

/**
 * @brief 谱面导入导出一致性测试工具
 *
 * 流程:
 * 1. 读取 <input> 为 map1
 * 2. 将 map1 写出到 <output>
 * 3. 读取 <output> 为 map2
 * 4. 对比 map1 与 map2
 */

bool compareBeatMaps(const MMM::BeatMap& m1, const MMM::BeatMap& m2)
{
    // 1. 比较 Metadata
    if ( m1.m_baseMapMetadata.track_count !=
         m2.m_baseMapMetadata.track_count ) {
        XERROR("Track count mismatch: {} vs {}",
               m1.m_baseMapMetadata.track_count,
               m2.m_baseMapMetadata.track_count);
        return false;
    }

    // 2. 比较 Timing
    if ( m1.m_timings.size() != m2.m_timings.size() ) {
        XERROR("Timing count mismatch: {} vs {}",
               m1.m_timings.size(),
               m2.m_timings.size());
        return false;
    }
    for ( size_t i = 0; i < m1.m_timings.size(); ++i ) {
        const auto& t1 = m1.m_timings[i];
        const auto& t2 = m2.m_timings[i];
        if ( std::abs(t1.m_timestamp - t2.m_timestamp) > 1e-3 ||
             std::abs(t1.m_bpm - t2.m_bpm) > 1e-3 ||
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

    // 3. 比较物件总数
    if ( m1.m_allNotes.size() != m2.m_allNotes.size() ) {
        XERROR("Note count mismatch: {} vs {}",
               m1.m_allNotes.size(),
               m2.m_allNotes.size());
        return false;
    }

    // 4. 比较具体的 Note 类型和数据 (简单抽样比对首尾)
    if ( !m1.m_allNotes.empty() ) {
        auto n1_first = m1.m_allNotes.front().get();
        auto n2_first = m2.m_allNotes.front().get();
        if ( n1_first.m_type != n2_first.m_type ||
             std::abs(n1_first.m_timestamp - n2_first.m_timestamp) > 1e-3 ) {
            XERROR("First note mismatch: type1={}, t1={} | type2={}, t2={}",
                   (int)n1_first.m_type,
                   n1_first.m_timestamp,
                   (int)n2_first.m_type,
                   n2_first.m_timestamp);
            return false;
        }
    }

    // 5. 原生 MMM 必须完整承接来源格式中的自动采样对象。
    if ( m1.m_audioSamples.size() != m2.m_audioSamples.size() ) {
        XERROR("Audio sample count mismatch: {} vs {}",
               m1.m_audioSamples.size(),
               m2.m_audioSamples.size());
        return false;
    }
    std::vector<const MMM::AudioSampleEvent*> samples1;
    std::vector<const MMM::AudioSampleEvent*> samples2;
    for ( const auto& sample : m1.m_audioSamples ) {
        samples1.push_back(&sample);
    }
    for ( const auto& sample : m2.m_audioSamples ) {
        samples2.push_back(&sample);
    }
    auto sortSamples = [](auto& samples) {
        std::sort(
            samples.begin(),
            samples.end(),
            [](const auto* lhs, const auto* rhs) {
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
    };
    sortSamples(samples1);
    sortSamples(samples2);
    for ( std::size_t index = 0; index < samples1.size(); ++index ) {
        const auto& lhs = *samples1[index];
        const auto& rhs = *samples2[index];
        if ( std::abs(lhs.m_timestamp - rhs.m_timestamp) > 1e-3 ||
             lhs.m_offsetMs != rhs.m_offsetMs || lhs.m_track != rhs.m_track ||
             lhs.m_audioResourceId != rhs.m_audioResourceId ||
             std::abs(lhs.m_volume - rhs.m_volume) > 1e-6F ) {
            XERROR("Audio sample mismatch at index {}", index);
            return false;
        }
    }

    return true;
}

int main(int argc, char* argv[])
{
    if ( argc < 3 ) {
        XERROR("Usage: MapConsistencyTest <input_file> <output_file>");
        return 1;
    }

    fs::path inputPath  = argv[1];
    fs::path outputPath = argv[2];

    XINFO("Testing map consistency: {}", inputPath.string());

    // 1. 第一次加载
    MMM::BeatMap map1 = MMM::BeatMap::loadFromFile(inputPath);
    if ( map1.m_timings.empty() && map1.m_allNotes.empty() &&
         map1.m_audioSamples.empty() ) {
        XERROR("Failed to load map1 or map is empty: {}", inputPath.string());
        return 1;
    }

    // 2. 导出
    if ( !map1.saveToFile(outputPath) ) {
        XERROR("Failed to save map to: {}", outputPath.string());
        return 1;
    }

    // 3. 第二次加载
    MMM::BeatMap map2 = MMM::BeatMap::loadFromFile(outputPath);
    if ( map2.m_timings.empty() && map2.m_allNotes.empty() &&
         map2.m_audioSamples.empty() ) {
        XERROR("Failed to load map2 (re-imported): {}", outputPath.string());
        return 1;
    }

    // 4. 逻辑比对
    if ( !compareBeatMaps(map1, map2) ) {
        XERROR("Consistency check failed for {}",
               inputPath.filename().string());
        return 1;
    }

    XINFO("Consistency test PASSED for {}", inputPath.filename().string());
    return 0;
}
