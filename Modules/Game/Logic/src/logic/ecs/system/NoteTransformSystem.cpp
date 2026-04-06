#include "logic/ecs/system/NoteTransformSystem.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"

namespace MMM::Logic::System
{

void NoteTransformSystem::update(entt::registry& registry,
                                 entt::registry& timelineRegistry,
                                 double          currentTime,
                                 const Config::EditorConfig& /* config */
)
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

        double noteAbsY = cache.getAbsY(note.m_timestamp);
        float  relY     = static_cast<float>(noteAbsY - currentAbsY);

        float minY = relY;
        float maxY = relY + 20.0f;

        if ( note.m_type == ::MMM::NoteType::HOLD ) {
            double endAbsY = cache.getAbsY(note.m_timestamp + note.m_duration);
            maxY           = static_cast<float>(endAbsY - currentAbsY);
        } else if ( note.m_type == ::MMM::NoteType::POLYLINE &&
                    !note.m_subNotes.empty() ) {
            for ( const auto& sub : note.m_subNotes ) {
                double subAbsY = cache.getAbsY(sub.timestamp);
                float  subRelY = static_cast<float>(subAbsY - currentAbsY);
                minY           = std::min(minY, subRelY);

                double subEndAbsY = cache.getAbsY(sub.timestamp + sub.duration);
                float subEndRelY = static_cast<float>(subEndAbsY - currentAbsY);
                maxY             = std::max(maxY, subEndRelY + 20.0f);
            }
        }

        // 仅在 NoteTransformSystem 中计算 Y 轴逻辑位置，X
        // 轴与宽度由渲染系统基于布局动态计算
        transform.m_pos.y  = minY;
        transform.m_size.y = maxY - minY;
    }
}

}  // namespace MMM::Logic::System
