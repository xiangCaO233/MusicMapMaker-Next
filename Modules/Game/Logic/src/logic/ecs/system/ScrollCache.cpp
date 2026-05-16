#include "logic/ecs/system/ScrollCache.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <cmath>

namespace MMM::Logic::System
{

void ScrollCache::rebuild(const entt::registry& timelineRegistry)
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

    const double BASE_SPEED = 500.0;  // 基准流速 (px/s)
    const auto&  visualConfig =
        EditorEngine::instance().getEditorConfig().visual;
    const double globalMultiplier = visualConfig.timelineZoom;
    const bool   isLinearMapping  = visualConfig.enableLinearScrollMapping;

    // 1. 获取基准 BPM
    double refBPM = 120.0;
    if ( auto session = EditorEngine::instance().getActiveSession() ) {
        if ( auto beatmap = session->getContext().currentBeatmap ) {
            refBPM = beatmap->m_baseMapMetadata.preference_bpm;
        }
    }
    if ( refBPM <= 0.0 ) {
        for ( const auto& entry : m_rebuildScratch ) {
            if ( entry.component->m_effect == ::MMM::TimingEffect::BPM ) {
                refBPM = entry.component->m_value;
                break;
            }
        }
    }
    if ( refBPM < 1.0 ) refBPM = 120.0;
    if ( refBPM > 1000000.0 ) refBPM = 1000000.0;

    // osu! 风格：流速 = min(bpm/refBPM, maxBpmRatio) × scrollMult × baseSpeed
    // 保留 BPM 倍率使得 timelineZoom 对高低 BPM 段落均有自然的缩放响应，
    // 同时通过上限制约极端 BPM (如 10000) 导致的速度爆发
    constexpr double maxBpmRatio = 20.0;

    auto calcSpeed = [&](double bpm, double sm) {
        if ( isLinearMapping ) {
            return BASE_SPEED * globalMultiplier;
        }
        if ( std::abs(refBPM) < 1e-6 ) return 0.0;
        double bpmRatio = std::clamp(std::abs(bpm) / refBPM, 0.1, maxBpmRatio);
        return bpmRatio * sm * BASE_SPEED * globalMultiplier;
    };

    double currentBPM        = refBPM;
    double currentScrollMult = 1.0;
    double lastTime = std::min(0.0, m_rebuildScratch[0].component->m_timestamp);
    double currentAbsY = 0.0;

    double currentSpeed = calcSpeed(currentBPM, currentScrollMult);
    newSegments.push_back({ lastTime, 0.0, currentSpeed, 0 });

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
            currentScrollMult            = 1.0;
        } else if ( tl->m_effect == ::MMM::TimingEffect::SCROLL ) {
            newSegments.back().effects |= SCROLL_EFFECT_SCROLL;
            newSegments.back().scrollEntity = entry.entity;
            newSegments.back().scrollValue  = tl->m_value;
            if ( tl->m_value < -1e-6 ) {
                currentScrollMult = -100.0 / tl->m_value;
            } else if ( tl->m_value >= 0 ) {
                currentScrollMult = tl->m_value;
            } else {
                currentScrollMult = 1.0;
            }
            if ( currentScrollMult > 10000.0 ) currentScrollMult = 10000.0;
        }

        currentSpeed             = calcSpeed(currentBPM, currentScrollMult);
        newSegments.back().speed = currentSpeed;
    }

    m_segments = std::move(newSegments);
    isDirty    = false;
}

double ScrollCache::getAbsY(double t) const
{
    const double DEFAULT_SPEED =
        500.0 * EditorEngine::instance().getEditorConfig().visual.timelineZoom;
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
    const double DEFAULT_SPEED =
        500.0 * EditorEngine::instance().getEditorConfig().visual.timelineZoom;
    if ( m_segments.empty() ) return absY / DEFAULT_SPEED;

    auto it = std::lower_bound(
        m_segments.begin(),
        m_segments.end(),
        absY,
        [](const ScrollSegment& seg, double val) { return seg.absY < val; });

    if ( it == m_segments.begin() ) {
        if ( std::abs(m_segments[0].speed) < 1e-6 ) return m_segments[0].time;
        return m_segments[0].time +
               (absY - m_segments[0].absY) / m_segments[0].speed;
    }
    --it;
    if ( std::abs(it->speed) < 1e-6 ) return it->time;
    return it->time + (absY - it->absY) / it->speed;
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

double ScrollCache::getSmoothedAbsY(double t, double alpha) const
{
    double raw = getAbsY(t);
    if ( !m_emaInitialized ) {
        m_smoothedAbsY     = raw;
        m_emaInitialized   = true;
        m_lastSmoothedTime = t;
        return raw;
    }
    // 防止同帧内多次调用叠加平滑
    if ( std::abs(t - m_lastSmoothedTime) < 1e-6 ) {
        return m_smoothedAbsY;
    }
    m_lastSmoothedTime = t;
    m_smoothedAbsY     = alpha * raw + (1.0 - alpha) * m_smoothedAbsY;
    return m_smoothedAbsY;
}

}  // namespace MMM::Logic::System
