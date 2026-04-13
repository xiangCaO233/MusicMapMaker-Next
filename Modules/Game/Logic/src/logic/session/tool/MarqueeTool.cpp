#include "logic/session/tool/MarqueeTool.h"
#include "logic/BeatmapSession.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/system/ScrollCache.h"

namespace MMM::Logic
{

void MarqueeTool::handleStartMarquee(BeatmapSession&        session,
                                     const CmdStartMarquee& cmd)
{
    session.m_isSelecting         = true;
    session.m_hasMarqueeSelection = true;
    session.m_marqueeIsAdditive   = cmd.isCtrlDown;

    // 如果不是加选模式，先清除当前所有选中框和实体选中
    if ( !session.m_marqueeIsAdditive ) {
        session.m_marqueeBoxes.clear();
        auto view = session.m_noteRegistry.view<InteractionComponent>();
        for ( auto entity : view ) {
            session.m_noteRegistry.get<InteractionComponent>(entity)
                .isSelected = false;
        }
    }

    auto* cache = session.m_timelineRegistry.ctx().find<System::ScrollCache>();
    if ( cache ) {
        auto it = session.m_cameras.find(cmd.cameraId);
        if ( it != session.m_cameras.end() ) {
            BeatmapSession::MarqueeBox newBox;
            newBox.cameraId = cmd.cameraId;

            float judgmentLineY = it->second.viewportHeight *
                                  session.m_lastConfig.visual.judgeline_pos;

            float renderScaleY = 1.0f;
            if ( cmd.cameraId == "Preview" ) {
                auto  itMain = session.m_cameras.find("Basic2DCanvas");
                float mainViewportHeight = itMain != session.m_cameras.end()
                                               ? itMain->second.viewportHeight
                                               : it->second.viewportHeight;

                float mainEffectiveH =
                    (session.m_lastConfig.visual.trackLayout.bottom -
                     session.m_lastConfig.visual.trackLayout.top) *
                    mainViewportHeight;
                float ty = session.m_lastConfig.visual.previewConfig.margin.top;
                float by =
                    it->second.viewportHeight -
                    session.m_lastConfig.visual.previewConfig.margin.bottom;
                float previewDrawH = by - ty;

                renderScaleY =
                    previewDrawH /
                    (mainEffectiveH *
                     session.m_lastConfig.visual.previewConfig.areaRatio);
            } else {
                renderScaleY = session.m_lastConfig.visual.noteScaleY;
            }

            double currentAbsY = cache->getAbsY(session.m_visualTime);
            double targetAbsY =
                currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
            newBox.startTime = cache->getTime(targetAbsY);
            newBox.endTime   = newBox.startTime;

            float leftX      = it->second.viewportWidth *
                               session.m_lastConfig.visual.trackLayout.left;
            float rightX     = it->second.viewportWidth *
                               session.m_lastConfig.visual.trackLayout.right;
            float trackAreaW = rightX - leftX;
            newBox.startTrack =
                (cmd.mouseX - leftX) / (trackAreaW / session.m_trackCount);
            newBox.endTrack = newBox.startTrack;

            session.m_marqueeBoxes.push_back(std::move(newBox));
        }
    }
}

void MarqueeTool::handleUpdateMarquee(BeatmapSession&         session,
                                      const CmdUpdateMarquee& cmd)
{
    if ( !session.m_isSelecting || session.m_marqueeBoxes.empty() ) return;
    auto& currentBox = session.m_marqueeBoxes.back();

    auto* cache = session.m_timelineRegistry.ctx().find<System::ScrollCache>();
    if ( cache ) {
        auto it = session.m_cameras.find(currentBox.cameraId);
        if ( it != session.m_cameras.end() ) {
            float judgmentLineY = it->second.viewportHeight *
                                  session.m_lastConfig.visual.judgeline_pos;

            float renderScaleY = 1.0f;
            if ( currentBox.cameraId == "Preview" ) {
                auto  itMain = session.m_cameras.find("Basic2DCanvas");
                float mainViewportHeight = itMain != session.m_cameras.end()
                                               ? itMain->second.viewportHeight
                                               : it->second.viewportHeight;

                float mainEffectiveH =
                    (session.m_lastConfig.visual.trackLayout.bottom -
                     session.m_lastConfig.visual.trackLayout.top) *
                    mainViewportHeight;
                float ty = session.m_lastConfig.visual.previewConfig.margin.top;
                float by =
                    it->second.viewportHeight -
                    session.m_lastConfig.visual.previewConfig.margin.bottom;
                float previewDrawH = by - ty;

                renderScaleY =
                    previewDrawH /
                    (mainEffectiveH *
                     session.m_lastConfig.visual.previewConfig.areaRatio);
            } else {
                renderScaleY = session.m_lastConfig.visual.noteScaleY;
            }

            double currentAbsY = cache->getAbsY(session.m_visualTime);
            double targetAbsY =
                currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
            currentBox.endTime = cache->getTime(targetAbsY);

            float leftX      = it->second.viewportWidth *
                               session.m_lastConfig.visual.trackLayout.left;
            float rightX     = it->second.viewportWidth *
                               session.m_lastConfig.visual.trackLayout.right;
            float trackAreaW = rightX - leftX;
            currentBox.endTrack =
                (cmd.mouseX - leftX) / (trackAreaW / session.m_trackCount);
        }
    }
}

void MarqueeTool::handleEndMarquee(BeatmapSession&      session,
                                   const CmdEndMarquee& cmd)
{
    session.m_isSelecting = false;
    // 如果框的大小极小，可以认为是一个无效框，清理掉
    if ( !session.m_marqueeBoxes.empty() ) {
        auto& lastBox = session.m_marqueeBoxes.back();
        if ( std::abs(lastBox.endTime - lastBox.startTime) < 0.001 &&
             std::abs(lastBox.endTrack - lastBox.startTrack) < 0.1 ) {
            session.m_marqueeBoxes.pop_back();
        }
    }
}

void MarqueeTool::handleRemoveMarqueeAt(BeatmapSession&           session,
                                        const CmdRemoveMarqueeAt& cmd)
{
    auto* cache = session.m_timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return;

    auto it = session.m_cameras.find(cmd.cameraId);
    if ( it == session.m_cameras.end() ) return;

    // 1. 计算点击位置对应的逻辑时间与轨道
    float judgmentLineY =
        it->second.viewportHeight * session.m_lastConfig.visual.judgeline_pos;

    float renderScaleY = 1.0f;
    if ( cmd.cameraId == "Preview" ) {
        auto  itMain             = session.m_cameras.find("Basic2DCanvas");
        float mainViewportHeight = itMain != session.m_cameras.end()
                                       ? itMain->second.viewportHeight
                                       : it->second.viewportHeight;
        float mainEffectiveH = (session.m_lastConfig.visual.trackLayout.bottom -
                                session.m_lastConfig.visual.trackLayout.top) *
                               mainViewportHeight;
        float ty = session.m_lastConfig.visual.previewConfig.margin.top;
        float by = it->second.viewportHeight -
                   session.m_lastConfig.visual.previewConfig.margin.bottom;
        float previewDrawH = by - ty;
        renderScaleY = previewDrawH /
                       (mainEffectiveH *
                        session.m_lastConfig.visual.previewConfig.areaRatio);
    } else {
        renderScaleY = session.m_lastConfig.visual.noteScaleY;
    }

    double currentAbsY = cache->getAbsY(session.m_visualTime);
    double targetAbsY =
        currentAbsY + (judgmentLineY - cmd.mouseY) / renderScaleY;
    double clickTime = cache->getTime(targetAbsY);

    float leftX =
        it->second.viewportWidth * session.m_lastConfig.visual.trackLayout.left;
    float rightX     = it->second.viewportWidth *
                       session.m_lastConfig.visual.trackLayout.right;
    float trackAreaW = rightX - leftX;
    float clickTrack =
        (cmd.mouseX - leftX) / (trackAreaW / session.m_trackCount);

    // 2. 逆序查找被点击的框
    for ( int i = static_cast<int>(session.m_marqueeBoxes.size()) - 1; i >= 0;
          --i ) {
        auto& box = session.m_marqueeBoxes[i];
        if ( box.cameraId != cmd.cameraId ) continue;

        double minTime  = std::min(box.startTime, box.endTime);
        double maxTime  = std::max(box.startTime, box.endTime);
        float  minTrack = std::min(box.startTrack, box.endTrack);
        float  maxTrack = std::max(box.startTrack, box.endTrack);

        if ( clickTime >= minTime && clickTime <= maxTime &&
             clickTrack >= minTrack && clickTrack <= maxTrack ) {
            session.m_marqueeBoxes.erase(session.m_marqueeBoxes.begin() + i);

            // 移除后需要重算一次所有物件的选中状态
            // 如果框选列表空了，清除所有选中（除非处于追加模式，但移除框通常意味着我们想改变选中结果）
            if ( session.m_marqueeBoxes.empty() ) {
                auto view = session.m_noteRegistry.view<InteractionComponent>();
                for ( auto entity : view ) {
                    session.m_noteRegistry.get<InteractionComponent>(entity)
                        .isSelected = false;
                }
            } else {
                session.updateMarqueeSelection(true);
            }
            return;
        }
    }
}

}  // namespace MMM::Logic
