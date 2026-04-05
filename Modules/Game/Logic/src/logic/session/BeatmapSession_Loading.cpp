#include "log/colorful-log.h"
#include "logic/BeatmapSession.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "mmm/beatmap/BeatMap.h"
#include <stb_image.h>

namespace MMM::Logic
{

void BeatmapSession::loadBeatmap(std::shared_ptr<MMM::BeatMap> beatmap)
{
    m_noteRegistry.clear();
    m_timelineRegistry.clear();

    m_isPlaying      = true;
    m_currentTime    = 0.0;
    m_currentBeatmap = beatmap;

    if ( !beatmap ) return;

    // 计算背景图片的绝对路径并获取尺寸
    m_bgSize    = glm::vec2(0.0f);
    auto bgPath = beatmap->m_baseMapMetadata.map_path.parent_path() /
                  beatmap->m_baseMapMetadata.main_cover_path;

    if ( !beatmap->m_baseMapMetadata.main_cover_path.empty() &&
         std::filesystem::exists(bgPath) ) {
        int w = 0, h = 0, comp = 0;
        if ( stbi_info(bgPath.string().c_str(), &w, &h, &comp) ) {
            m_bgSize = glm::vec2(static_cast<float>(w), static_cast<float>(h));
        }
    }

    m_trackCount = beatmap->m_baseMapMetadata.track_count;
    if ( m_trackCount <= 0 ) m_trackCount = 12;  // 默认值

    // 清空缓存上下文，以确保重新构建
    if ( auto* cache = m_timelineRegistry.ctx().find<System::ScrollCache>() ) {
        cache->isDirty = true;
    }

    // 根据传入的 BeatMap 构建 ECS 实体
    // 1. 加载 Timing 点
    for ( const auto& timing : beatmap->m_timings ) {
        auto entity = m_timelineRegistry.create();
        m_timelineRegistry.emplace<TimelineComponent>(
            entity,
            timing.m_timestamp / 1000.0,  // 毫秒转秒
            timing.m_timingEffect,
            timing.m_timingEffectParameter);
    }

    // 用于追踪子物件，防止在折线之外重复绘制
    std::unordered_map<const ::MMM::Note*, entt::entity> noteToEntity;

    // 2. 加载普通音符 (Notes)
    for ( const auto& note : beatmap->m_noteData.notes ) {
        auto entity         = m_noteRegistry.create();
        noteToEntity[&note] = entity;

        int track = static_cast<int>(note.m_track);

        m_noteRegistry.emplace<NoteComponent>(
            entity,
            note.m_type,
            note.m_timestamp / 1000.0,  // 毫秒转秒
            0.0,
            track);

        m_noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 3. 加载长键 (Holds)
    for ( const auto& hold : beatmap->m_noteData.holds ) {
        auto entity         = m_noteRegistry.create();
        noteToEntity[&hold] = entity;

        int track = static_cast<int>(hold.m_track);

        m_noteRegistry.emplace<NoteComponent>(
            entity,
            hold.m_type,
            hold.m_timestamp / 1000.0,  // 毫秒转秒
            hold.m_duration / 1000.0,   // 毫秒转秒
            track);

        m_noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 4. 加载滑键 (Flicks)
    for ( const auto& flick : beatmap->m_noteData.flicks ) {
        auto entity          = m_noteRegistry.create();
        noteToEntity[&flick] = entity;

        int track = static_cast<int>(flick.m_track);

        m_noteRegistry.emplace<NoteComponent>(entity,
                                              flick.m_type,
                                              flick.m_timestamp / 1000.0,
                                              0.0,
                                              track,
                                              flick.m_dtrack);

        m_noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 5. 加载折线 (Polylines)
    for ( const auto& polyline : beatmap->m_noteData.polylines ) {
        auto entity = m_noteRegistry.create();

        int track = static_cast<int>(polyline.m_track);

        auto& comp = m_noteRegistry.emplace<NoteComponent>(
            entity, polyline.m_type, polyline.m_timestamp / 1000.0, 0.0, track);

        // 填充子物件并标记它们为 SubNote
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            const auto& subNote = subNoteRef.get();

            // 标记原始实体为 SubNote，防止独立绘制
            if ( auto it = noteToEntity.find(&subNote);
                 it != noteToEntity.end() ) {
                auto& subComp = m_noteRegistry.get<NoteComponent>(it->second);
                subComp.m_isSubNote      = true;
                subComp.m_parentPolyline = entity;
            }

            NoteComponent::SubNote sn;
            sn.type       = subNote.m_type;
            sn.timestamp  = subNote.m_timestamp / 1000.0;
            sn.trackIndex = static_cast<int>(subNote.m_track);
            sn.duration   = 0.0;
            sn.dtrack     = 0;

            if ( subNote.m_type == ::MMM::NoteType::HOLD ) {
                const auto& h = static_cast<const ::MMM::Hold&>(subNote);
                sn.duration   = h.m_duration / 1000.0;
            } else if ( subNote.m_type == ::MMM::NoteType::FLICK ) {
                const auto& f = static_cast<const ::MMM::Flick&>(subNote);
                sn.dtrack     = f.m_dtrack;
            }
            comp.m_subNotes.push_back(sn);
        }

        m_noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    XINFO(
        "Loaded new BeatMap with {} notes, {} holds, {} flicks, {} polylines "
        "and {} timings.",
        beatmap->m_noteData.notes.size(),
        beatmap->m_noteData.holds.size(),
        beatmap->m_noteData.flicks.size(),
        beatmap->m_noteData.polylines.size(),
        beatmap->m_timings.size());
}

}  // namespace MMM::Logic
