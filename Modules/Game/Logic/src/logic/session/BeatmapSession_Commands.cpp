#include "audio/AudioManager.h"
#include "logic/BeatmapSession.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
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
                                    cache->getAbsY(m_visualTime);
                                double targetAbsY =
                                    currentAbsY + (judgmentLineY - arg.mouseY);
                                double targetTime = cache->getTime(targetAbsY);

                                // 获取已排序的 BPM 事件用于磁吸
                                std::vector<const TimelineComponent*> bpmEvents;
                                auto                                  tlView =
                                    m_timelineRegistry
                                        .view<const TimelineComponent>();
                                for ( auto entity : tlView ) {
                                    const auto& tl =
                                        tlView.get<const TimelineComponent>(
                                            entity);
                                    if ( tl.m_effect ==
                                         ::MMM::TimingEffect::BPM ) {
                                        bpmEvents.push_back(&tl);
                                    }
                                }
                                std::stable_sort(
                                    bpmEvents.begin(),
                                    bpmEvents.end(),
                                    [](const TimelineComponent* a,
                                       const TimelineComponent* b) {
                                        return a->m_timestamp < b->m_timestamp;
                                    });

                                // 磁吸处理
                                auto snap = getSnapResult(targetTime,
                                                          arg.mouseY,
                                                          it->second,
                                                          m_lastConfig,
                                                          bpmEvents);
                                if ( snap.isSnapped ) {
                                    targetTime = snap.snappedTime;
                                }

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
                        // 拖拽结束后重新构建打击事件列表，确保音效同步
                        rebuildHitEvents();
                    }
                    m_draggedEntity = entt::null;
                } else if constexpr ( std::is_same_v<T, CmdUpdateTrackCount> ) {
                    m_trackCount = arg.trackCount;
                    if ( m_currentBeatmap ) {
                        m_currentBeatmap->m_baseMapMetadata.track_count =
                            arg.trackCount;
                    }
                } else if constexpr ( std::is_same_v<T, CmdSeek> ) {
                    double totalTime =
                        Audio::AudioManager::instance().getTotalTime();
                    m_currentTime = std::clamp(arg.time, 0.0, totalTime);
                    m_syncClock.reset(m_currentTime);
                    Audio::AudioManager::instance().seek(m_currentTime);
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
                } else if constexpr ( std::is_same_v<T, CmdScroll> ) {
                    float wheel = arg.wheel;
                    if ( m_lastConfig.settings.reverseScroll ) {
                        wheel = -wheel;
                    }

                    double targetTime = m_currentTime;

                    if ( m_lastConfig.settings.scrollSnap ) {
                        // 滚动磁吸逻辑
                        int beatDivisor = m_lastConfig.settings.beatDivisor;
                        if ( beatDivisor <= 0 ) beatDivisor = 4;

                        // 获取所有 BPM 事件
                        std::vector<const TimelineComponent*> bpmEvents;
                        auto                                  tlView =
                            m_timelineRegistry.view<const TimelineComponent>();
                        for ( auto entity : tlView ) {
                            const auto& tl =
                                tlView.get<const TimelineComponent>(entity);
                            if ( tl.m_effect == ::MMM::TimingEffect::BPM ) {
                                bpmEvents.push_back(&tl);
                            }
                        }

                        if ( !bpmEvents.empty() ) {
                            std::sort(bpmEvents.begin(),
                                      bpmEvents.end(),
                                      [](const auto* a, const auto* b) {
                                          return a->m_timestamp <
                                                 b->m_timestamp;
                                      });

                            // 找到当前时间所在的 BPM 区间
                            size_t currentIdx = 0;
                            for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
                                if ( m_currentTime >=
                                     bpmEvents[i]->m_timestamp ) {
                                    currentIdx = i;
                                } else {
                                    break;
                                }
                            }

                            const auto* currentBPM = bpmEvents[currentIdx];
                            double      bpmVal     = currentBPM->m_value;
                            double      beatDuration =
                                60.0 / (bpmVal > 0 ? bpmVal : 120.0);
                            double stepDuration = beatDuration / beatDivisor;

                            double relativeTime =
                                m_currentTime - currentBPM->m_timestamp;
                            double stepCount = relativeTime / stepDuration;

                            // 根据滚轮方向决定是向上取整还是向下取整，以确保“下一个”或“上一个”
                            if ( wheel > 0 ) {
                                // 向上滚动 (时间减小)
                                // 我们想要寻找上一个分拍线
                                targetTime = currentBPM->m_timestamp +
                                             std::floor(stepCount - 0.001) *
                                                 stepDuration;
                            } else {
                                // 向下滚动 (时间增大)
                                // 我们想要寻找下一个分拍线
                                targetTime =
                                    currentBPM->m_timestamp +
                                    std::ceil(stepCount + 0.001) * stepDuration;
                            }
                        } else {
                            // 无 BPM 事件，回退到普通滚动
                            double step = 0.25;
                            if ( arg.isShiftDown ) step *= 3.0;
                            targetTime = m_currentTime -
                                         static_cast<double>(wheel) * step;
                        }
                    } else {
                        // 普通滚动逻辑
                        double step = 0.25;
                        if ( arg.isShiftDown ) step *= 3.0;
                        targetTime =
                            m_currentTime - static_cast<double>(wheel) * step;
                    }

                    double totalTime =
                        Audio::AudioManager::instance().getTotalTime();
                    m_currentTime = std::clamp(targetTime, 0.0, totalTime);
                    m_syncClock.reset(m_currentTime);
                    Audio::AudioManager::instance().seek(m_currentTime);
                    syncHitIndex();
                    m_hitFXSystem.clearActiveEffects();
                }
            },
            cmd);
    }
}

}  // namespace MMM::Logic