#include "logic/ecs/system/ScrollCache.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace MMM::Logic::System
{

static bool isMalodyEffect(const TimelineComponent& tl, const std::string& name)
{
    if ( auto it = tl.m_metadata.timing_properties.find(
             ::MMM::TimingMetadataType::MALODY);
         it != tl.m_metadata.timing_properties.end() ) {
        if ( auto effectIt = it->second.find("effect");
             effectIt != it->second.end() ) {
            return effectIt->second == "\"" + name + "\"";
        }
    }
    return false;
}

static bool hasMalodyMetadata(const TimelineComponent& tl)
{
    return tl.m_metadata.timing_properties.contains(
        ::MMM::TimingMetadataType::MALODY);
}

/// @brief 根据时间线注册表重建滚动缓存。
/// @warning 逻辑热路径低频分支：会完整遍历和排序时间线，只能在 isDirty
/// 时执行，严禁每 update 无条件调用。
void ScrollCache::rebuild(const entt::registry&       timelineRegistry,
                          const Config::EditorConfig& config)
{
    m_rebuildScratch.clear();
    auto tlView = timelineRegistry.view<const TimelineComponent>();
    m_rebuildScratch.reserve(tlView.size());

    for ( auto entity : tlView ) {
        m_rebuildScratch.push_back(
            { entity, &tlView.get<const TimelineComponent>(entity) });
    }

    if ( m_rebuildScratch.empty() ) {
        m_segments.clear();
        m_absYRangeIndex.clear();
        isDirty = false;
        return;
    }

    // 排序逻辑：时间戳升序；时间戳相同时，BPM 类型优先于 SCROLL 类型
    std::stable_sort(
        m_rebuildScratch.begin(),
        m_rebuildScratch.end(),
        [](const auto& a, const auto& b) {
            if ( std::abs(a.component->m_timestamp - b.component->m_timestamp) >
                 1e-9 ) {
                return a.component->m_timestamp < b.component->m_timestamp;
            }
            if ( a.component->m_effect != b.component->m_effect ) {
                return a.component->m_effect == ::MMM::TimingEffect::BPM;
            }
            return false;  // 保持原始顺序
        });

    std::vector<ScrollSegment> newSegments;
    newSegments.reserve(m_rebuildScratch.size() + 1);

    const double BASE_SPEED      = 500.0;
    const auto&  visualConfig    = config.visual;
    const bool   isLinearMapping = visualConfig.enableLinearScrollMapping;
    const bool   enableEffects   = !isLinearMapping;
    const double timelineZoom    = visualConfig.timelineZoom;
    m_lastZoom                   = timelineZoom;

    // 1. 完整版 osu! 逻辑：计算 Most Common BPM 作为基准，并获取
    // SliderMultiplier
    double refBPM           = 120.0;
    double sliderMultiplier = 1.0;
    if ( auto session = EditorEngine::instance().getActiveSession() ) {
        if ( auto beatmap = session->getContext().currentBeatmap ) {
            // 尝试获取 SliderMultiplier (默认为 1.4)
            sliderMultiplier = beatmap->m_metadata.get_value<double>(
                MapMetadataType::OSU, "Difficulty::SliderMultiplier", 1.4);

            // 自动计算最常见的 BPM (持续时间最长)
            std::map<double, double> bpmDurations;
            double                   lastBpmTime   = 0.0;
            double                   currentBpmVal = -1.0;
            for ( const auto& entry : m_rebuildScratch ) {
                if ( entry.component->m_effect == ::MMM::TimingEffect::BPM ) {
                    if ( currentBpmVal > 0 ) {
                        bpmDurations[currentBpmVal] +=
                            (entry.component->m_timestamp - lastBpmTime);
                    }
                    currentBpmVal = entry.component->m_value;
                    lastBpmTime   = entry.component->m_timestamp;
                }
            }
            // 加上最后一个段落到末尾的时间 (假设谱面时长)
            bpmDurations[currentBpmVal] +=
                (beatmap->m_baseMapMetadata.map_length - lastBpmTime);

            double maxDuration = -1.0;
            for ( const auto& [bpm, dur] : bpmDurations ) {
                if ( dur > maxDuration ) {
                    maxDuration = dur;
                    refBPM      = bpm;
                }
            }
        }
    }
    if ( refBPM < 1.0 ) refBPM = 120.0;

    double currentBPM = refBPM;

    // osu! MultiplierControlPoint:
    //   Multiplier = Velocity × ScrollSpeed × BaseBeatLength / BeatLength
    //            = 1.0  × scrollMult  × (60000/refBPM) / (60000/bpm)
    //            = scrollMult × bpm / refBPM
    //   speed     = Multiplier × scrollLength / timeRange
    //            = scrollMult × bpm/refBPM × BASE_SPEED × timelineZoom
    auto calcSpeed = [&](double bpm, double sm) {
        if ( isLinearMapping ) {
            return BASE_SPEED * timelineZoom;
        }
        double ratio = std::clamp(bpm / refBPM, 0.0, 1000000.0);
        return ratio * sm * sliderMultiplier * BASE_SPEED * timelineZoom;
    };

    // osu! 关键：SV 跨 BPM 红线继承，不重置。仅绿线显式修改 ScrollSpeed。
    double currentScrollMult = 1.0;
    double currentHs         = 1.0;
    double lastTime = std::min(0.0, m_rebuildScratch[0].component->m_timestamp);
    double currentAbsY = 0.0;

    double currentSpeed = calcSpeed(currentBPM, currentScrollMult);
    newSegments.push_back({ lastTime, 0.0, currentSpeed, 0 });
    newSegments.back().hs      = currentHs;
    newSegments.back().hsValue = currentHs;

    for ( const auto& entry : m_rebuildScratch ) {
        const auto* tl = entry.component;
        if ( tl->m_timestamp > lastTime ) {
            double dt = tl->m_timestamp - lastTime;
            currentAbsY += dt * currentSpeed;
            lastTime = tl->m_timestamp;
            newSegments.push_back({ lastTime, currentAbsY, currentSpeed, 0 });
        }

        if ( tl->m_effect == ::MMM::TimingEffect::BPM ) {
            newSegments.back().effects |= SCROLL_EFFECT_BPM;
            newSegments.back().bpmEntity = entry.entity;
            newSegments.back().bpmValue  = tl->m_value;
            currentBPM                   = tl->m_value;
            if ( enableEffects && !hasMalodyMetadata(*tl) ) {
                // osu! 红线会重置 SV；Malody 的 BPM 不改变 effect 状态。
                currentScrollMult = 1.0;
            }
        } else if ( tl->m_effect == ::MMM::TimingEffect::SCROLL ) {
            newSegments.back().effects |= SCROLL_EFFECT_SCROLL;
            newSegments.back().scrollEntity = entry.entity;
            newSegments.back().scrollValue  = tl->m_value;
            if ( enableEffects ) {
                if ( hasMalodyMetadata(*tl) ) {
                    currentScrollMult = tl->m_value;
                } else if ( tl->m_value < -1e-6 ) {
                    currentScrollMult = -100.0 / tl->m_value;
                } else if ( tl->m_value >= 0 ) {
                    currentScrollMult = tl->m_value;
                } else {
                    currentScrollMult = 1.0;
                }
            }
        } else if ( tl->m_effect == ::MMM::TimingEffect::JUMP ) {
            newSegments.back().effects |= SCROLL_EFFECT_JUMP;
            newSegments.back().jumpEntity = entry.entity;
            newSegments.back().jumpValue  = tl->m_value;
            if ( enableEffects ) {
                currentAbsY += (tl->m_value / 1000.0) * currentSpeed;
                newSegments.back().absY = currentAbsY;
            }
        } else if ( tl->m_effect == ::MMM::TimingEffect::HS ) {
            newSegments.back().effects |= SCROLL_EFFECT_HS;
            newSegments.back().hsEntity = entry.entity;
            newSegments.back().hsValue  = tl->m_value;
            if ( enableEffects ) {
                currentHs = tl->m_value;
            }
        }

        currentSpeed             = calcSpeed(currentBPM, currentScrollMult);
        newSegments.back().speed = currentSpeed;
        newSegments.back().hs    = currentHs;
    }

    m_segments = std::move(newSegments);
    rebuildAbsYRangeIndex();
    isDirty = false;
}

std::pair<double, double> ScrollCache::getSegmentAbsYRange(
    std::size_t index) const
{
    const auto& seg = m_segments[index];
    if ( index + 1 < m_segments.size() ) {
        double nextAbsY = m_segments[index + 1].absY;
        return { std::min(seg.absY, nextAbsY), std::max(seg.absY, nextAbsY) };
    }

    if ( seg.speed >= 0.0 ) {
        return { seg.absY, std::numeric_limits<double>::infinity() };
    }
    return { -std::numeric_limits<double>::infinity(), seg.absY };
}

void ScrollCache::rebuildAbsYRangeIndex()
{
    m_absYRangeIndex.clear();
    m_absYRangeIndex.reserve(m_segments.size());

    for ( std::size_t i = 0; i < m_segments.size(); ++i ) {
        const auto& seg = m_segments[i];
        if ( std::abs(seg.speed) < 1e-9 ) continue;

        auto [minAbsY, maxAbsY] = getSegmentAbsYRange(i);
        m_absYRangeIndex.push_back({ minAbsY, maxAbsY, i });
    }

    std::stable_sort(m_absYRangeIndex.begin(),
                     m_absYRangeIndex.end(),
                     [](const AbsYRangeEntry& a, const AbsYRangeEntry& b) {
                         if ( std::abs(a.minAbsY - b.minAbsY) > 1e-9 )
                             return a.minAbsY < b.minAbsY;
                         if ( std::abs(a.maxAbsY - b.maxAbsY) > 1e-9 )
                             return a.maxAbsY < b.maxAbsY;
                         return a.segmentIndex < b.segmentIndex;
                     });
}

double ScrollCache::getAbsY(double t) const
{
    const double DEFAULT_SPEED = 500.0 * m_lastZoom;
    if ( m_segments.empty() ) return t * DEFAULT_SPEED;

    auto it = std::upper_bound(
        m_segments.begin(),
        m_segments.end(),
        t,
        [](double val, const ScrollSegment& seg) { return val < seg.time; });

    if ( it == m_segments.begin() ) {
        // 如果 t 比第一个点还早，按第一个点的速度回溯
        return m_segments[0].absY +
               (t - m_segments[0].time) * m_segments[0].speed;
    }
    --it;
    return it->absY + (t - it->time) * it->speed;
}

double ScrollCache::getTime(double absY) const
{
    const double DEFAULT_SPEED = 500.0 * m_lastZoom;
    if ( m_segments.empty() ) return absY / DEFAULT_SPEED;

    constexpr double EPS       = 1e-6;
    std::size_t      bestIndex = std::numeric_limits<std::size_t>::max();
    auto             endIt =
        std::upper_bound(m_absYRangeIndex.begin(),
                         m_absYRangeIndex.end(),
                         absY + EPS,
                         [](double value, const AbsYRangeEntry& entry) {
                             return value < entry.minAbsY;
                         });

    for ( auto it = m_absYRangeIndex.begin(); it != endIt; ++it ) {
        if ( it->maxAbsY < absY - EPS ) continue;
        if ( it->segmentIndex < bestIndex ) {
            bestIndex = it->segmentIndex;
        }
    }

    if ( bestIndex != std::numeric_limits<std::size_t>::max() ) {
        const auto& seg = m_segments[bestIndex];
        return seg.time + (absY - seg.absY) / seg.speed;
    }

    const auto& first = m_segments.front();
    const auto& last  = m_segments.back();
    const auto& edge =
        std::abs(absY - first.absY) < std::abs(absY - last.absY) ? first : last;
    if ( std::abs(edge.speed) < 1e-6 ) return edge.time;
    return edge.time + (absY - edge.absY) / edge.speed;
}

double ScrollCache::getSpeedAt(double t) const
{
    if ( m_segments.empty() ) return 1.0;
    auto it = std::upper_bound(
        m_segments.begin(),
        m_segments.end(),
        t,
        [](double val, const ScrollSegment& seg) { return val < seg.time; });

    if ( it == m_segments.begin() ) return m_segments[0].speed;
    --it;
    return it->speed;
}

double ScrollCache::getHsAt(double t) const
{
    if ( m_segments.empty() ) return 1.0;
    auto it = std::upper_bound(
        m_segments.begin(),
        m_segments.end(),
        t,
        [](double val, const ScrollSegment& seg) { return val < seg.time; });

    if ( it == m_segments.begin() ) return m_segments[0].hs;
    --it;
    return it->hs;
}

double ScrollCache::getDisplayDelta(double t, double currentAbsY,
                                    double anchorTime) const
{
    const double DEFAULT_SPEED = 500.0 * m_lastZoom;
    if ( m_segments.empty() ) {
        return (t * DEFAULT_SPEED - currentAbsY);
    }

    auto it = std::upper_bound(
        m_segments.begin(),
        m_segments.end(),
        t,
        [](double val, const ScrollSegment& seg) { return val < seg.time; });

    const ScrollSegment* seg = nullptr;
    if ( it == m_segments.begin() ) {
        seg = &m_segments.front();
    } else {
        seg = &(*std::prev(it));
    }

    double absY = seg->absY + (t - seg->time) * seg->speed;
    double hs =
        (std::abs(t - anchorTime) <= 1e-9) ? seg->hs : getHsAt(anchorTime);
    return (absY - currentAbsY) * hs;
}

bool ScrollCache::hasJumpEffects() const
{
    return std::any_of(
        m_segments.begin(), m_segments.end(), [](const ScrollSegment& seg) {
            return (seg.effects & SCROLL_EFFECT_JUMP) != 0;
        });
}

double ScrollCache::getMaxJumpSecondsInRange(double startTime, double endTime,
                                             double padding) const
{
    if ( startTime > endTime ) {
        std::swap(startTime, endTime);
    }

    const double queryStart = startTime - padding;
    const double queryEnd   = endTime + padding;
    double       maxJump    = 0.0;

    for ( const auto& seg : m_segments ) {
        if ( seg.time < queryStart ) continue;
        if ( seg.time > queryEnd ) break;
        if ( (seg.effects & SCROLL_EFFECT_JUMP) == 0 ) continue;
        maxJump = std::max(maxJump, std::abs(seg.jumpValue) / 1000.0);
    }

    return maxJump;
}

std::vector<std::pair<double, double>> ScrollCache::getTimeRangesForAbsYWindow(
    double minAbsY, double maxAbsY) const
{
    std::vector<std::pair<double, double>> ranges;
    if ( m_segments.empty() ) return ranges;

    if ( minAbsY > maxAbsY ) {
        std::swap(minAbsY, maxAbsY);
    }

    auto appendRange = [&](double startTime, double endTime) {
        if ( startTime > endTime ) {
            std::swap(startTime, endTime);
        }
        if ( endTime < startTime - 1e-9 ) return;

        if ( !ranges.empty() && startTime <= ranges.back().second + 1e-6 ) {
            ranges.back().second = std::max(ranges.back().second, endTime);
            return;
        }

        ranges.emplace_back(startTime, endTime);
    };

    std::vector<std::size_t> candidateIndices;
    auto                     rangeEndIt =
        std::upper_bound(m_absYRangeIndex.begin(),
                         m_absYRangeIndex.end(),
                         maxAbsY + 1e-6,
                         [](double value, const AbsYRangeEntry& entry) {
                             return value < entry.minAbsY;
                         });

    for ( auto it = m_absYRangeIndex.begin(); it != rangeEndIt; ++it ) {
        if ( it->maxAbsY < minAbsY - 1e-6 ) continue;
        candidateIndices.push_back(it->segmentIndex);
    }

    std::sort(candidateIndices.begin(), candidateIndices.end());
    candidateIndices.erase(
        std::unique(candidateIndices.begin(), candidateIndices.end()),
        candidateIndices.end());

    for ( std::size_t i : candidateIndices ) {
        const auto& seg = m_segments[i];
        if ( std::abs(seg.speed) < 1e-9 ) continue;

        const bool hasNext      = i + 1 < m_segments.size();
        double     t0           = seg.time + (minAbsY - seg.absY) / seg.speed;
        double     t1           = seg.time + (maxAbsY - seg.absY) / seg.speed;
        double     overlapStart = std::min(t0, t1);
        double     overlapEnd   = std::max(t0, t1);

        if ( hasNext ) {
            double segEndTime = m_segments[i + 1].time;
            double segEndAbsY = seg.absY + (segEndTime - seg.time) * seg.speed;
            double segMinAbsY = std::min(seg.absY, segEndAbsY);
            double segMaxAbsY = std::max(seg.absY, segEndAbsY);

            if ( segMaxAbsY < minAbsY - 1e-6 || segMinAbsY > maxAbsY + 1e-6 ) {
                continue;
            }

            overlapStart = std::max(overlapStart, seg.time);
            overlapEnd   = std::min(overlapEnd, segEndTime);
        } else {
            overlapStart = std::max(overlapStart, seg.time);
        }

        if ( overlapEnd >= overlapStart - 1e-9 ) {
            appendRange(overlapStart, overlapEnd);
        }
    }

    return ranges;
}

std::vector<std::pair<double, double>> ScrollCache::getTimeSlices(
    double startTime, double endTime) const
{
    std::vector<std::pair<double, double>> slices;
    if ( startTime >= endTime ) return slices;

    auto it = std::upper_bound(
        m_segments.begin(),
        m_segments.end(),
        startTime,
        [](double val, const ScrollSegment& seg) { return val < seg.time; });

    double current = startTime;
    while ( current < endTime ) {
        double nextTime = (it != m_segments.end()) ? it->time : endTime;
        if ( nextTime > endTime ) nextTime = endTime;
        if ( nextTime > current ) {
            slices.emplace_back(current, nextTime);
        }
        current = nextTime;
        if ( it != m_segments.end() ) ++it;
    }
    return slices;
}

}  // namespace MMM::Logic::System
