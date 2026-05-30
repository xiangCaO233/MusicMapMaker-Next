#include "logic/ecs/system/NoteTransformSystem.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"

namespace MMM::Logic::System
{

/// @brief 更新音符逻辑坐标缓存。
/// @warning 逻辑热路径：每个 Session update 调用；完整 registry sort/view
/// 遍历只允许在 cacheDirty 或 forceRebuild 时执行。
void NoteTransformSystem::update(entt::registry&             registry,
                                 entt::registry&             timelineRegistry,
                                 double                      currentTime,
                                 const Config::EditorConfig& config,
                                 MMM::BeatMap* beatmap, bool forceRebuild)
{
    auto& cache      = timelineRegistry.ctx().get<ScrollCache>();
    bool  cacheDirty = cache.isDirty;
    if ( cache.isDirty ) {
        cache.rebuild(timelineRegistry, config, beatmap);
    }

    if ( !cacheDirty && !forceRebuild ) {
        return;
    }

    registry.sort<NoteComponent>(
        [](const NoteComponent& lhs, const NoteComponent& rhs) {
            return lhs.m_timestamp < rhs.m_timestamp;
        });

    double currentAbsY = cache.getAbsY(currentTime);

    auto noteView = registry.view<TransformComponent, const NoteComponent>();
    for ( auto entity : noteView ) {
        auto&       transform = noteView.get<TransformComponent>(entity);
        const auto& note      = noteView.get<const NoteComponent>(entity);

        double noteAbsY = cache.getAbsY(note.m_timestamp);
        double noteHs   = cache.getHsAt(note.m_timestamp);
        float  relY     = static_cast<float>((noteAbsY - currentAbsY) * noteHs);

        float minY = relY;
        float maxY = relY + 20.0f;

        if ( note.m_type == ::MMM::NoteType::HOLD ) {
            double endAbsY = cache.getAbsY(note.m_timestamp + note.m_duration);
            maxY = static_cast<float>((endAbsY - currentAbsY) * noteHs);
        } else if ( note.m_type == ::MMM::NoteType::POLYLINE &&
                    !note.m_subNotes.empty() ) {
            for ( const auto& sub : note.m_subNotes ) {
                double subAbsY = cache.getAbsY(sub.timestamp);
                double subHs   = cache.getHsAt(sub.timestamp);
                float  subRelY =
                    static_cast<float>((subAbsY - currentAbsY) * subHs);
                minY = std::min(minY, subRelY);

                double subEndAbsY = cache.getAbsY(sub.timestamp + sub.duration);
                float  subEndRelY =
                    static_cast<float>((subEndAbsY - currentAbsY) * subHs);
                maxY = std::max(maxY, subEndRelY + 20.0f);
            }
        }

        // 仅在 NoteTransformSystem 中计算 Y 轴逻辑位置，X
        // 轴与宽度由渲染系统基于布局动态计算
        transform.m_pos.y  = minY;
        transform.m_size.y = maxY - minY;
    }
}

}  // namespace MMM::Logic::System
