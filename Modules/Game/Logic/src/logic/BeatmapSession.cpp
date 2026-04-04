#include "logic/BeatmapSession.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/NoteSystem.h"
#include "mmm/beatmap/BeatMap.h"
#include <glm/glm.hpp>

// 独立函数，用于 entt 信号回调，标记 ScrollCache 为脏状态
static void markScrollCacheDirty(entt::registry& reg, entt::entity)
{
    if ( auto* cache = reg.ctx().find<MMM::Logic::System::ScrollCache>() ) {
        cache->isDirty = true;
    }
}

namespace MMM::Logic
{

BeatmapSession::BeatmapSession()
{
    // 初始化时注册 TimelineComponent 的增删改信号，并注入 ScrollCache 上下文
    m_timelineRegistry.ctx().emplace<System::ScrollCache>();
    m_timelineRegistry.on_construct<TimelineComponent>()
        .connect<&markScrollCacheDirty>();
    m_timelineRegistry.on_update<TimelineComponent>()
        .connect<&markScrollCacheDirty>();
    m_timelineRegistry.on_destroy<TimelineComponent>()
        .connect<&markScrollCacheDirty>();
}

void BeatmapSession::loadBeatmap(std::shared_ptr<MMM::BeatMap> beatmap)
{
    m_noteRegistry.clear();
    m_timelineRegistry.clear();

    m_isPlaying   = true;
    m_currentTime = 0.0;

    if ( !beatmap ) return;

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

    // 2. 加载普通音符 (Notes)
    for ( const auto& note : beatmap->m_noteData.notes ) {
        auto entity = m_noteRegistry.create();

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
        auto entity = m_noteRegistry.create();

        int track = static_cast<int>(hold.m_track);

        m_noteRegistry.emplace<NoteComponent>(
            entity,
            hold.m_type,
            hold.m_timestamp / 1000.0,  // 毫秒转秒
            hold.m_duration / 1000.0,   // 毫秒转秒
            track);

        // TODO: TransformComponent for Holds might need different size
        // calculation
        m_noteRegistry.emplace<TransformComponent>(
            entity,
            glm::vec2(track * 60.0f + 20.0f, 0.0f),
            glm::vec2(50.0f, 20.0f));
    }

    // 4. 加载滑键 (Flicks)
    for ( const auto& flick : beatmap->m_noteData.flicks ) {
        auto entity = m_noteRegistry.create();

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

        // 填充子物件
        for ( const auto& subNoteRef : polyline.m_subNotes ) {
            const auto&            subNote = subNoteRef.get();
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

    // DEBUG print first 5 notes
    int debugCount = 0;
    for ( const auto& note : beatmap->m_noteData.notes ) {
        if ( debugCount++ < 5 ) {
            XINFO("Note {}: track={}, timestamp={}",
                  debugCount,
                  note.m_track,
                  note.m_timestamp);
        }
    }
}

void BeatmapSession::pushCommand(LogicCommand&& cmd)
{
    m_commandQueue.enqueue(std::move(cmd));
}

void BeatmapSession::update(double dt)
{
    processCommands();

    if ( m_isPlaying ) {
        m_currentTime += dt;
    }

    updateECSAndRender();
}

void BeatmapSession::processCommands()
{
    LogicCommand cmd;
    // 不断尝试出队直到队列为空
    while ( m_commandQueue.try_dequeue(cmd) ) {
        std::visit(
            [this](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr ( std::is_same_v<T, CmdUpdateViewport> ) {
                    if ( m_cameras.find(arg.cameraId) == m_cameras.end() ) {
                        m_cameras[arg.cameraId] =
                            CameraInfo{ arg.cameraId, arg.width, arg.height };
                    } else {
                        m_cameras[arg.cameraId].viewportWidth  = arg.width;
                        m_cameras[arg.cameraId].viewportHeight = arg.height;
                    }
                } else if constexpr ( std::is_same_v<T, CmdSetPlayState> ) {
                    m_isPlaying = arg.isPlaying;
                } else if constexpr ( std::is_same_v<T, CmdLoadBeatmap> ) {
                    loadBeatmap(arg.beatmap);
                } else if constexpr ( std::is_same_v<T, CmdSetHoveredEntity> ) {
                    // 先清空所有实体的 Hover 状态 (简易处理，未来可优化)
                    auto view = m_noteRegistry.view<InteractionComponent>();
                    for ( auto entity : view ) {
                        m_noteRegistry.get<InteractionComponent>(entity)
                            .isHovered = false;
                    }

                    // 设置新的 Hover 实体
                    if ( arg.entity != entt::null ) {
                        // 如果没有该组件则添加
                        if ( !m_noteRegistry.all_of<InteractionComponent>(
                                 arg.entity) ) {
                            m_noteRegistry.emplace<InteractionComponent>(
                                arg.entity);
                        }
                        m_noteRegistry.get<InteractionComponent>(arg.entity)
                            .isHovered = true;
                    }
                }
            },
            cmd);
    }
}

void BeatmapSession::updateECSAndRender()
{
    // 1. 调用 ECS System 更新全局物理位置 (Logical Transform)
    // 注意：我们将 judgmentLineY 的计算延后到渲染阶段，所以这里的
    // TransformSystem 其实只计算逻辑距离 (relativeDistance)
    System::NoteTransformSystem::update(
        m_noteRegistry, m_timelineRegistry, m_currentTime, 0.0f);

    // 2. 遍历所有注册的视口 (Camera) 进行独立的视口剔除和坐标映射
    for ( const auto& [cameraId, camera] : m_cameras ) {
        // 从 EditorEngine 获取该 Camera 专属的缓冲
        auto syncBuffer = EditorEngine::instance().getSyncBuffer(cameraId);
        if ( !syncBuffer ) continue;

        RenderSnapshot* snapshot = syncBuffer->getWorkingSnapshot();
        if ( !snapshot ) continue;

        snapshot->clear();

        // 假设判定线在画布底部偏上 100 像素的位置
        float judgmentLineY = camera.viewportHeight - 100.0f;

        // 3. 调用 ECS System 针对当前 Camera 生成渲染快照
        System::NoteRenderSystem::generateSnapshot(m_noteRegistry,
                                                   m_timelineRegistry,
                                                   snapshot,
                                                   m_currentTime,
                                                   camera.viewportHeight,
                                                   judgmentLineY);

        // 4. 提交专属快照
        syncBuffer->pushWorkingSnapshot();
    }
}

}  // namespace MMM::Logic
