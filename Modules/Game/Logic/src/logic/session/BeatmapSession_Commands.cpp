#include "audio/AudioManager.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TransformComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "mmm/beatmap/BeatMap.h"
#include <variant>

namespace MMM::Logic
{

void BeatmapSession::processCommands()
{
    LogicCommand cmd;
    // 不断尝试出队直到队列为空
    while ( m_commandQueue.try_dequeue(cmd) ) {
        std::visit(
            [this](auto&& arg) {
                using T = std::decay_t<decltype(arg)>;
                if constexpr ( std::is_same_v<T, CmdUpdateEditorConfig> ) {
                    m_lastConfig = arg.config;
                    // 当配置更新时 (例如全局流速倍率修改)，强制标记流速缓存为
                    // Dirty，以便在下一帧重新计算
                    auto* cache =
                        m_timelineRegistry.ctx().find<System::ScrollCache>();
                    if ( cache ) {
                        cache->isDirty = true;
                    }
                } else if constexpr ( std::is_same_v<T, CmdUpdateViewport> ) {
                    if ( m_cameras.find(arg.cameraId) == m_cameras.end() ) {
                        m_cameras[arg.cameraId] =
                            CameraInfo{ arg.cameraId, arg.width, arg.height };
                    } else {
                        m_cameras[arg.cameraId].viewportWidth  = arg.width;
                        m_cameras[arg.cameraId].viewportHeight = arg.height;
                    }
                } else if constexpr ( std::is_same_v<T, CmdSetPlayState> ) {
                    m_isPlaying = arg.isPlaying;
                    if ( m_isPlaying ) {
                        Audio::AudioManager::instance().play();
                        m_syncClock.reset(m_currentTime);
                        // 开始播放时，确保索引同步并清理可能的残留特效
                        syncHitIndex();
                        m_hitFXSystem.clearActiveEffects();
                    } else {
                        Audio::AudioManager::instance().pause();
                        // 当且仅当暂停播放时，从音频引擎读取一次时间，令 ECS
                        // 进度与实际播放位置一致
                        m_currentTime =
                            Audio::AudioManager::instance().getCurrentTime();
                    }
                } else if constexpr ( std::is_same_v<T, CmdLoadBeatmap> ) {
                    loadBeatmap(arg.beatmap);
                } else if constexpr ( std::is_same_v<T, CmdSetHoveredEntity> ) {
                    auto view = m_noteRegistry.view<InteractionComponent>();
                    for ( auto entity : view ) {
                        m_noteRegistry.get<InteractionComponent>(entity)
                            .isHovered = false;
                    }
                    if ( arg.entity != entt::null ) {
                        if ( !m_noteRegistry.all_of<InteractionComponent>(
                                 arg.entity) ) {
                            m_noteRegistry.emplace<InteractionComponent>(
                                arg.entity);
                        }
                        m_noteRegistry.get<InteractionComponent>(arg.entity)
                            .isHovered = true;
                    }
                } else if constexpr ( std::is_same_v<T, CmdSelectEntity> ) {
                    if ( arg.clearOthers ) {
                        auto view = m_noteRegistry.view<InteractionComponent>();
                        for ( auto entity : view ) {
                            m_noteRegistry.get<InteractionComponent>(entity)
                                .isSelected = false;
                        }
                    }
                    if ( arg.entity != entt::null ) {
                        if ( !m_noteRegistry.all_of<InteractionComponent>(
                                 arg.entity) ) {
                            m_noteRegistry.emplace<InteractionComponent>(
                                arg.entity);
                        }
                        auto& ic = m_noteRegistry.get<InteractionComponent>(
                            arg.entity);
                        ic.isSelected = !ic.isSelected;
                    }
                } else if constexpr ( std::is_same_v<T, CmdStartDrag> ) {
                    if ( arg.entity != entt::null &&
                         m_noteRegistry.valid(arg.entity) ) {
                        m_draggedEntity = arg.entity;
                        m_dragCameraId  = arg.cameraId;
                        if ( !m_noteRegistry.all_of<InteractionComponent>(
                                 arg.entity) ) {
                            m_noteRegistry.emplace<InteractionComponent>(
                                arg.entity);
                        }
                        m_noteRegistry.get<InteractionComponent>(arg.entity)
                            .isDragging = true;
                    }
                } else if constexpr ( std::is_same_v<T, CmdUpdateDrag> ) {
                    if ( m_draggedEntity != entt::null &&
                         m_noteRegistry.valid(m_draggedEntity) ) {
                        auto it = m_cameras.find(arg.cameraId);
                        if ( it != m_cameras.end() ) {
                            float judgmentLineY =
                                it->second.viewportHeight *
                                m_lastConfig.visual.judgeline_pos;
                            auto* cache = m_timelineRegistry.ctx()
                                              .find<System::ScrollCache>();
                            if ( cache ) {
                                double currentAbsY =
                                    cache->getAbsY(m_currentTime);
                                double targetAbsY =
                                    currentAbsY + (judgmentLineY - arg.mouseY);
                                double targetTime = cache->getTime(targetAbsY);

                                if ( auto* note =
                                         m_noteRegistry.try_get<NoteComponent>(
                                             m_draggedEntity) ) {
                                    note->m_timestamp = targetTime;
                                    float leftX =
                                        it->second.viewportWidth *
                                        m_lastConfig.visual.trackLayout.left;
                                    float rightX =
                                        it->second.viewportWidth *
                                        m_lastConfig.visual.trackLayout.right;
                                    float trackAreaW = rightX - leftX;
                                    float noteW = trackAreaW / m_trackCount;
                                    int   track = static_cast<int>(std::round(
                                        (arg.mouseX - leftX - noteW / 2.0f) /
                                        noteW));
                                    track =
                                        std::clamp(track, 0, m_trackCount - 1);
                                    note->m_trackIndex = track;

                                    if ( auto* trans =
                                             m_noteRegistry
                                                 .try_get<TransformComponent>(
                                                     m_draggedEntity) ) {
                                        trans->m_pos.x = leftX + track * noteW;
                                    }
                                }
                            }
                        }
                    }
                } else if constexpr ( std::is_same_v<T, CmdEndDrag> ) {
                    if ( m_draggedEntity != entt::null &&
                         m_noteRegistry.valid(m_draggedEntity) ) {
                        if ( m_noteRegistry.all_of<InteractionComponent>(
                                 m_draggedEntity) ) {
                            m_noteRegistry
                                .get<InteractionComponent>(m_draggedEntity)
                                .isDragging = false;
                        }
                    }
                    m_draggedEntity = entt::null;
                } else if constexpr ( std::is_same_v<T, CmdUpdateTrackCount> ) {
                    m_trackCount = arg.trackCount;
                    if ( m_currentBeatmap ) {
                        m_currentBeatmap->m_baseMapMetadata.track_count =
                            arg.trackCount;
                    }
                } else if constexpr ( std::is_same_v<T, CmdSeek> ) {
                    m_currentTime = arg.time;
                    m_syncClock.reset(arg.time);
                    Audio::AudioManager::instance().seek(arg.time);
                    // Seek 时同步索引并清理残留特效
                    syncHitIndex();
                    m_hitFXSystem.clearActiveEffects();
                } else if constexpr ( std::is_same_v<T, CmdSetPlaybackSpeed> ) {
                    Audio::AudioManager::instance().setPlaybackSpeed(arg.speed);
                } else if constexpr ( std::is_same_v<T, CmdChangeTool> ) {
                    m_currentTool = arg.tool;
                } else if constexpr ( std::is_same_v<T, CmdSetMousePosition> ) {
                    m_mouseCameraId   = arg.cameraId;
                    m_mouseX          = arg.mouseX;
                    m_mouseY          = arg.mouseY;
                    m_isMouseInCanvas = arg.isHovering;
                }
            },
            cmd);
    }
}

}  // namespace MMM::Logic