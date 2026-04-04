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
                            // 1. 获取判定线位置
                            float judgmentLineY = it->second.viewportHeight *
                                                  m_lastConfig.judgeline_pos;

                            // 2. 将屏幕 Y 映射回逻辑绝对 Y
                            // noteAbsY = currentAbsY + judgmentLineY - screenY
                            auto* cache = m_timelineRegistry.ctx()
                                              .find<System::ScrollCache>();
                            if ( cache ) {
                                double currentAbsY =
                                    cache->getAbsY(m_currentTime);
                                double targetAbsY =
                                    currentAbsY + (judgmentLineY - arg.mouseY);

                                // 3. 反查时间戳
                                double targetTime = cache->getTime(targetAbsY);

                                // 4. 更新 NoteComponent
                                if ( auto* note =
                                         m_noteRegistry.try_get<NoteComponent>(
                                             m_draggedEntity) ) {
                                    note->m_timestamp = targetTime;

                                    // 计算轨道区域
                                    float leftX = it->second.viewportWidth *
                                                  m_lastConfig.trackLayout.left;
                                    float rightX =
                                        it->second.viewportWidth *
                                        m_lastConfig.trackLayout.right;
                                    float trackAreaW = rightX - leftX;
                                    float noteW = trackAreaW / m_trackCount;

                                    // 更新轨道
                                    int track = static_cast<int>(std::round(
                                        (arg.mouseX - leftX - noteW / 2.0f) /
                                        noteW));
                                    track =
                                        std::clamp(track, 0, m_trackCount - 1);
                                    note->m_trackIndex = track;

                                    // 同步更新 TransformComponent (X 坐标)
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
                }
            },
            cmd);
    }
}

}  // namespace MMM::Logic
