#include "canvas/TimelineCanvas.h"
#include "config/AppConfig.h"
#include "imgui.h"
#include <algorithm>

namespace MMM::Canvas
{

void TimelineCanvas::handleRightClick(const ImVec2& size)
{
    auto&  visual        = Config::AppConfig::instance().getVisualConfig();
    float  judgmentLineY = size.y * visual.judgeline_pos;
    ImVec2 canvasPos     = ImGui::GetItemRectMin();
    ImVec2 mousePos      = ImGui::GetMousePos();
    float  localMouseY   = mousePos.y - canvasPos.y;

    const auto& segments = m_currentSnapshot->scrollSegments;
    if ( segments.empty() ) {
        float  zoom     = visual.timelineZoom;
        double speed    = 500.0 * zoom;
        m_createTimeRaw = (judgmentLineY - localMouseY) / speed +
                          m_currentSnapshot->currentTime;
    } else {
        // 寻找 currentTime 对应的 AbsY
        auto it = std::upper_bound(
            segments.begin(),
            segments.end(),
            m_currentSnapshot->currentTime,
            [](double val, const Logic::System::ScrollSegment& seg) {
                return val < seg.time;
            });
        double currentAbsY = 0;
        if ( it == segments.begin() ) {
            currentAbsY = segments[0].absY +
                          (m_currentSnapshot->currentTime - segments[0].time) *
                              segments[0].speed;
        } else {
            auto prev = std::prev(it);
            currentAbsY =
                prev->absY +
                (m_currentSnapshot->currentTime - prev->time) * prev->speed;
        }

        double targetAbsY = currentAbsY + (judgmentLineY - localMouseY);

        // 寻找 targetAbsY 对应的 Time
        auto itTime =
            std::lower_bound(segments.begin(),
                             segments.end(),
                             targetAbsY,
                             [](const Logic::System::ScrollSegment& seg,
                                double val) { return seg.absY < val; });

        if ( itTime == segments.begin() ) {
            if ( std::abs(segments[0].speed) < 1e-6 )
                m_createTimeRaw = segments[0].time;
            else
                m_createTimeRaw =
                    segments[0].time +
                    (targetAbsY - segments[0].absY) / segments[0].speed;
        } else {
            auto prev = std::prev(itTime);
            if ( std::abs(prev->speed) < 1e-6 )
                m_createTimeRaw = prev->time;
            else
                m_createTimeRaw =
                    prev->time + (targetAbsY - prev->absY) / prev->speed;
        }
    }

    // 磁吸逻辑
    m_isTimeSnapped     = false;
    m_createTimeSnapped = m_createTimeRaw;
    float minItemDist   = visual.snapThreshold;
    for ( const auto& el : m_currentSnapshot->timelineElements ) {
        if ( std::abs(el.y - localMouseY) < minItemDist ) {
            m_createTimeSnapped = el.time;
            m_isTimeSnapped     = true;
            minItemDist         = std::abs(el.y - localMouseY);
        }
    }

    m_isCreatePopupOpen = true;
    m_createTimeManual =
        m_isTimeSnapped ? m_createTimeSnapped : m_createTimeRaw;
    ImGui::OpenPopup("TimelineCreateEvent");
}

}  // namespace MMM::Canvas
