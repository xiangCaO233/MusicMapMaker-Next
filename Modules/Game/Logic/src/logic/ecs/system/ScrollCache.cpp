#include "logic/ecs/system/ScrollCache.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>
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
            if ( !hasMalodyMetadata(*tl) ) {
                // osu! 红线会重置 SV；Malody 的 BPM 不改变 effect 状态。
                currentScrollMult = 1.0;
            }
        } else if ( tl->m_effect == ::MMM::TimingEffect::SCROLL ) {
            newSegments.back().effects |= SCROLL_EFFECT_SCROLL;
            newSegments.back().scrollEntity = entry.entity;
            newSegments.back().scrollValue  = tl->m_value;
            if ( isMalodyEffect(*tl, "scroll") ) {
                currentScrollMult = tl->m_value;
            } else if ( tl->m_value < -1e-6 ) {
                currentScrollMult = -100.0 / tl->m_value;
            } else if ( tl->m_value >= 0 ) {
                currentScrollMult = tl->m_value;
            } else {
                currentScrollMult = 1.0;
            }
            currentScrollMult =
                std::clamp(currentScrollMult, -10000.0, 10000.0);
        } else if ( tl->m_effect == ::MMM::TimingEffect::JUMP ) {
            newSegments.back().effects |= SCROLL_EFFECT_JUMP;
            newSegments.back().jumpEntity = entry.entity;
            newSegments.back().jumpValue  = tl->m_value;
            currentAbsY += (tl->m_value / 1000.0) * currentSpeed;
            newSegments.back().absY = currentAbsY;
        } else if ( tl->m_effect == ::MMM::TimingEffect::HS ) {
            newSegments.back().effects |= SCROLL_EFFECT_HS;
            newSegments.back().hsEntity = entry.entity;
            newSegments.back().hsValue  = tl->m_value;
            currentHs                   = tl->m_value;
        }

        currentSpeed             = calcSpeed(currentBPM, currentScrollMult);
        newSegments.back().speed = currentSpeed;
        newSegments.back().hs    = currentHs;
    }

    m_segments = std::move(newSegments);
    isDirty    = false;
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

    for ( size_t i = 0; i < m_segments.size(); ++i ) {
        const auto& seg = m_segments[i];
        if ( std::abs(seg.speed) < 1e-6 ) continue;

        double nextAbsY = seg.absY;
        if ( i + 1 < m_segments.size() ) {
            nextAbsY = m_segments[i + 1].absY;
        } else {
            nextAbsY = seg.absY + seg.speed;
        }

        const double minY = std::min(seg.absY, nextAbsY);
        const double maxY = std::max(seg.absY, nextAbsY);
        if ( absY >= minY - 1e-6 && absY <= maxY + 1e-6 ) {
            return seg.time + (absY - seg.absY) / seg.speed;
        }
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
    return (getAbsY(t) - currentAbsY) * getHsAt(anchorTime);
}

}  // namespace MMM::Logic::System
