#include "logic/ecs/system/ScrollCache.h"
#include "logic/ecs/components/TimelineComponent.h"
#include <algorithm>

namespace MMM::Logic::System
{

void ScrollCache::rebuild(const entt::registry& timelineRegistry)
{
    // 获取并按时间排序时间线点
    std::vector<const TimelineComponent*> timings;
    auto tlView = timelineRegistry.view<const TimelineComponent>();
    for ( auto entity : tlView ) {
        timings.push_back(&tlView.get<const TimelineComponent>(entity));
    }
    std::stable_sort(
        timings.begin(), timings.end(), [](const auto* a, const auto* b) {
            return a->m_timestamp < b->m_timestamp;
        });

    m_segments.clear();

    if ( !timings.empty() ) {
        double currentBPM = 120.0;
        double currentSV  = 1.0;
        double lastTime   = timings[0]->m_timestamp;

        // 初始参数预扫描
        for ( const auto* tl : timings ) {
            if ( tl->m_timestamp > lastTime ) break;
            if ( tl->m_effect == ::MMM::TimingEffect::BPM ) {
                currentBPM = tl->m_value;
                currentSV  = 1.0;
            } else if ( tl->m_effect == ::MMM::TimingEffect::SCROLL ) {
                currentSV = tl->m_value < 0 ? -100.0 / tl->m_value : 1.0;
            }
        }

        double currentSpeed = currentBPM * 5.0 * currentSV;
        double currentAbsY  = 0.0;
        m_segments.push_back({ lastTime, currentAbsY, currentSpeed });

        for ( const auto* tl : timings ) {
            if ( tl->m_timestamp > lastTime ) {
                double dt = tl->m_timestamp - lastTime;
                currentAbsY += dt * currentSpeed;
                lastTime = tl->m_timestamp;
                m_segments.push_back({ lastTime, currentAbsY, currentSpeed });
            }

            if ( tl->m_effect == ::MMM::TimingEffect::BPM ) {
                currentBPM = tl->m_value;
                currentSV  = 1.0;
            } else if ( tl->m_effect == ::MMM::TimingEffect::SCROLL ) {
                currentSV = tl->m_value < 0 ? -100.0 / tl->m_value : 1.0;
            }
            currentSpeed            = currentBPM * 5.0 * currentSV;
            m_segments.back().speed = currentSpeed;
        }
    }
    isDirty = false;
}

double ScrollCache::getAbsY(double t) const
{
    if ( m_segments.empty() ) return t * 500.0;

    auto it = std::upper_bound(
        m_segments.begin(),
        m_segments.end(),
        t,
        [](double val, const ScrollSegment& seg) { return val < seg.time; });

    if ( it == m_segments.begin() ) {
        return m_segments[0].absY +
               (t - m_segments[0].time) * m_segments[0].speed;
    }
    --it;
    return it->absY + (t - it->time) * it->speed;
}

}  // namespace MMM::Logic::System
