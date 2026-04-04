#include "logic/ecs/system/NoteTransformSystem.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"

namespace MMM::Logic::System
{

void NoteTransformSystem::update(entt::registry& registry,
                                 entt::registry& timelineRegistry,
                                 double currentTime, float /* judgmentLineY */)
{
    auto& cache = timelineRegistry.ctx().get<ScrollCache>();
    if ( cache.isDirty ) {
        cache.rebuild(timelineRegistry);
    }

    double currentAbsY = cache.getAbsY(currentTime);

    auto noteView = registry.view<TransformComponent, const NoteComponent>();
    for ( auto entity : noteView ) {
        auto&       transform = noteView.get<TransformComponent>(entity);
        const auto& note      = noteView.get<const NoteComponent>(entity);

        // 基础轨道信息 (w=50)
        float startX = note.m_trackIndex * 60.0f + 20.0f;
        float noteW  = 50.0f;

        double noteAbsY = cache.getAbsY(note.m_timestamp);
        float  relY     = static_cast<float>(noteAbsY - currentAbsY);

        float minX = startX;
        float maxX = startX + noteW;
        float minY = relY;
        float maxY = relY + 20.0f;

        if ( note.m_type == ::MMM::NoteType::HOLD ) {
            double endAbsY = cache.getAbsY(note.m_timestamp + note.m_duration);
            maxY           = static_cast<float>(endAbsY - currentAbsY);
        } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
            if ( note.m_dtrack != 0 ) {
                float targetX =
                    (note.m_trackIndex + note.m_dtrack) * 60.0f + 20.0f;
                minX = std::min(minX, targetX);
                maxX = std::max(maxX, targetX + noteW);
            }
        } else if ( note.m_type == ::MMM::NoteType::POLYLINE &&
                    !note.m_subNotes.empty() ) {
            for ( const auto& sub : note.m_subNotes ) {
                float subX = sub.trackIndex * 60.0f + 20.0f;
                minX       = std::min(minX, subX);
                maxX       = std::max(maxX, subX + noteW);

                double subAbsY = cache.getAbsY(sub.timestamp);
                float  subRelY = static_cast<float>(subAbsY - currentAbsY);
                minY           = std::min(minY, subRelY);

                double subEndAbsY = cache.getAbsY(sub.timestamp + sub.duration);
                float subEndRelY = static_cast<float>(subEndAbsY - currentAbsY);
                maxY             = std::max(maxY, subEndRelY + 20.0f);
            }
        }

        transform.m_pos.x  = minX;
        transform.m_pos.y  = minY;
        transform.m_size.x = maxX - minX;
        transform.m_size.y = maxY - minY;
    }
}

}  // namespace MMM::Logic::System
