#include "logic/ecs/system/ScrollCache.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include <algorithm>
#include <cmath>

namespace MMM::Logic::System
{

void ScrollCache::rebuild(const entt::registry& timelineRegistry)
{
    m_segments.clear();
    const double BASE_SPEED = 500.0;  // 基准流速
    const auto&  visualConfig =
        EditorEngine::instance().getEditorConfig().visual;
    const double globalMultiplier = visualConfig.timelineZoom;
    const bool   isLinearMapping  = visualConfig.enableLinearScrollMapping;

    // 获取并排序时间线点
    std::vector<const TimelineComponent*> timings;
    auto tlView = timelineRegistry.view<const TimelineComponent>();
    for ( auto entity : tlView ) {
        timings.push_back(&tlView.get<const TimelineComponent>(entity));
    }

    if ( timings.empty() ) {
        isDirty = false;
        return;
    }

    // 排序逻辑：时间戳升序；时间戳相同时，BPM 类型优先于 SCROLL 类型
    std::stable_sort(
        timings.begin(), timings.end(), [](const auto* a, const auto* b) {
            if ( std::abs(a->m_timestamp - b->m_timestamp) > 1e-9 ) {
                return a->m_timestamp < b->m_timestamp;
            }
            if ( a->m_effect != b->m_effect ) {
                return a->m_effect == ::MMM::TimingEffect::BPM;
            }
            return false;  // 保持原始顺序
        });

    // 1. 查找基准 BPM (第一个出现的 BPM 类型点)
    double refBPM = 120.0;
    for ( const auto* tl : timings ) {
        if ( tl->m_effect == ::MMM::TimingEffect::BPM ) {
            refBPM = tl->m_value;
            break;
        }
    }

    // 2. 初始参数
    double currentBPM        = refBPM;
    double currentScrollMult = 1.0;

    // 如果第一个点在 0ms 之后，我们需要从 0ms 开始的一段默认速度
    // 如果第一个点就在 0ms
    // 或更早，这个初始段会在后续循环中被同步时间戳的点修正速度
    double lastTime    = std::min(0.0, timings[0]->m_timestamp);
    double currentAbsY = 0.0;

    auto calcSpeed = [&](double bpm, double sm) {
        if ( isLinearMapping ) {
            return BASE_SPEED * globalMultiplier;
        }
        if ( std::abs(refBPM) < 1e-6 ) return 0.0;
        return (bpm / refBPM) * sm * BASE_SPEED * globalMultiplier;
    };

    double currentSpeed = calcSpeed(currentBPM, currentScrollMult);
    m_segments.push_back({ lastTime, 0.0, currentSpeed, 0 });

    for ( const auto* tl : timings ) {
        // 如果时间戳推进了，则结算上一段的积分
        if ( tl->m_timestamp > lastTime ) {
            double dt = tl->m_timestamp - lastTime;
            currentAbsY += dt * currentSpeed;
            lastTime = tl->m_timestamp;
            m_segments.push_back({ lastTime, currentAbsY, currentSpeed, 0 });
        }

        // 累计当前时间戳的效果类型
        if ( tl->m_effect == ::MMM::TimingEffect::BPM ) {
            m_segments.back().effects |= SCROLL_EFFECT_BPM;
            currentBPM = tl->m_value;
        } else if ( tl->m_effect == ::MMM::TimingEffect::SCROLL ) {
            m_segments.back().effects |= SCROLL_EFFECT_SCROLL;
            if ( tl->m_value < 0 ) {
                currentScrollMult = -100.0 / tl->m_value;
            } else {
                currentScrollMult = tl->m_value;
            }
        }

        // 更新当前路段速度
        currentSpeed            = calcSpeed(currentBPM, currentScrollMult);
        m_segments.back().speed = currentSpeed;
    }

    isDirty = false;
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

}  // namespace MMM::Logic::System
