#include "audio/AudioManager.h"
#include "logic/BeatmapSession.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/NoteTransformSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "mmm/beatmap/BeatMap.h"
#include <numeric>

namespace MMM::Logic
{

void BeatmapSession::updateECSAndRender(const Config::EditorConfig& config)
{
    // 1. 调用 ECS System 更新全局物理位置 (Logical Transform)
    // 注意：物理位置更新应基于逻辑时间 m_currentTime
    System::NoteTransformSystem::update(
        m_noteRegistry, m_timelineRegistry, m_currentTime, config);

    // 筛选出所有 BPM 标记供后续视口处理（磁吸、智能拟合等）
    std::vector<const TimelineComponent*> bpmEvents;
    auto tlView = m_timelineRegistry.view<const TimelineComponent>();
    for ( auto entity : tlView ) {
        const auto& tl = tlView.get<const TimelineComponent>(entity);
        if ( tl.m_effect == ::MMM::TimingEffect::BPM ) {
            bpmEvents.push_back(&tl);
        }
    }
    std::stable_sort(
        bpmEvents.begin(),
        bpmEvents.end(),
        [](const TimelineComponent* a, const TimelineComponent* b) {
            return a->m_timestamp < b->m_timestamp;
        });

    // 0. 如果存在框选区域，更新选中状态
    if ( m_isSelecting || !m_marqueeBoxes.empty() ) {
        updateMarqueeSelection();
    }

    // 2. 遍历所有注册的视口 (Camera) 进行独立的视口剔除和坐标映射
    for ( auto& [cameraId, camera] : m_cameras ) {
        // 从 EditorEngine 获取该 Camera 专属的缓冲
        auto syncBuffer = EditorEngine::instance().getSyncBuffer(cameraId);
        if ( !syncBuffer ) continue;

        RenderSnapshot* snapshot = syncBuffer->getWorkingSnapshot();
        if ( !snapshot ) continue;

        snapshot->clear();

        // 注入该 Camera 特有的 UV 映射到快照
        snapshot->uvMap     = EditorEngine::instance().getAtlasUVMap(cameraId);
        snapshot->isPlaying = m_isPlaying;
        snapshot->currentTime = m_visualTime;  // 快照使用视觉平滑时间
        snapshot->totalTime   = Audio::AudioManager::instance().getTotalTime();
        snapshot->hasBeatmap  = (m_currentBeatmap != nullptr);

        if ( m_currentBeatmap ) {
            auto bgPath =
                m_currentBeatmap->m_baseMapMetadata.map_path.parent_path() /
                m_currentBeatmap->m_baseMapMetadata.main_cover_path;
            snapshot->backgroundPath = bgPath.string();
            snapshot->bgSize         = m_bgSize;
        }

        // --- 注入交互状态 ---
        snapshot->currentTool = m_currentTool;
        snapshot->isHoveringCanvas =
            m_isMouseInCanvas && (m_mouseCameraId == cameraId);

        // 核心修复：预览区的拖拽状态广播
        // 如果预览区正在拖拽，所有视口的渲染快照都需要知道预览区当前的悬停时间点。
        snapshot->isPreviewDragging =
            m_isDragging &&
            (m_dragCameraId == "Preview" || m_mouseCameraId == "Preview");
        snapshot->previewHoverTime = m_previewHoverTime;

        // --- 注入框选状态 ---
        snapshot->isSelecting = m_isSelecting;
        if ( m_isSelecting && !m_marqueeBoxes.empty() ) {
            snapshot->activeSelectionCameraId = m_marqueeBoxes.back().cameraId;
        }

        for ( const auto& box : m_marqueeBoxes ) {
            RenderSnapshot::MarqueeBoxSnapshot boxSnap;
            boxSnap.startTime  = box.startTime;
            boxSnap.endTime    = box.endTime;
            boxSnap.startTrack = box.startTrack;
            boxSnap.endTrack   = box.endTrack;
            boxSnap.cameraId   = box.cameraId;
            snapshot->marqueeBoxes.push_back(boxSnap);
        }

        if ( snapshot->isHoveringCanvas ) {
            auto* cache = m_timelineRegistry.ctx().find<System::ScrollCache>();
            if ( cache ) {
                float judgmentLineY =
                    camera.viewportHeight * config.visual.judgeline_pos;

                double currentAbsY = cache->getAbsY(m_visualTime);
                double deltaY      = (judgmentLineY - m_lastMousePos.y);

                float renderScaleY = config.visual.noteScaleY;
                // 核心修复：预览区的坐标是经过压缩的，计算时间时需要除以缩放比例
                if ( cameraId == "Preview" || cameraId == "PreviewCanvas" ) {
                    float previewMainHeight = 1000.0f;
                    auto  itMainPreview     = m_cameras.find("Basic2DCanvas");
                    if ( itMainPreview != m_cameras.end() ) {
                        previewMainHeight =
                            itMainPreview->second.viewportHeight;
                    }

                    float mainEffectiveH = (config.visual.trackLayout.bottom -
                                            config.visual.trackLayout.top) *
                                           previewMainHeight;

                    // 计算预览区的有效绘图高度（扣除边距）
                    float previewDrawH =
                        camera.viewportHeight -
                        (config.visual.previewConfig.margin.top +
                         config.visual.previewConfig.margin.bottom);

                    renderScaleY =
                        previewDrawH / (mainEffectiveH *
                                        config.visual.previewConfig.areaRatio);
                }

                if ( std::abs(renderScaleY) > 0.0001f ) {
                    deltaY /= renderScaleY;
                }

                double targetAbsY     = currentAbsY + deltaY;
                snapshot->hoveredTime = cache->getTime(targetAbsY);

                // 计算轨道
                float leftX =
                    camera.viewportWidth * config.visual.trackLayout.left;
                float rightX =
                    camera.viewportWidth * config.visual.trackLayout.right;
                float trackAreaW = rightX - leftX;
                float singleTrackW =
                    trackAreaW / static_cast<float>(m_trackCount);

                int track = static_cast<int>(
                    std::floor((m_lastMousePos.x - leftX) / singleTrackW));
                snapshot->hoveredTrack = std::clamp(track, 0, m_trackCount - 1);

                // --- 磁吸拍线时间戳预览 ---
                auto snap = getSnapResult(snapshot->hoveredTime,
                                          m_lastMousePos.y,
                                          camera,
                                          config,
                                          bpmEvents);
                if ( snap.isSnapped ) {
                    snapshot->isSnapped          = true;
                    snapshot->snappedTime        = snap.snappedTime;
                    snapshot->snappedNumerator   = snap.numerator;
                    snapshot->snappedDenominator = snap.denominator;
                }
                snapshot->currentBeatDivisor = config.settings.beatDivisor;

                // --- 预览区悬停状态 ---
                if ( cameraId == "Preview" ) {
                    snapshot->isPreviewHovered  = true;
                    snapshot->isPreviewDragging = m_isDragging;
                    snapshot->previewHoverY     = m_lastMousePos.y;

                    // 核心逻辑：拖动预览区时，主画布应该渲染拖拽处的内容
                    snapshot->previewHoverTime = snapshot->hoveredTime;
                    m_previewHoverTime         = snapshot->hoveredTime;
                }

                // --- 智能拟合：计算当前悬停物件的最简分拍 ---
                auto interView = m_noteRegistry.view<InteractionComponent>();
                for ( auto entity : interView ) {
                    const auto& inter =
                        interView.get<InteractionComponent>(entity);
                    if ( inter.isHovered ) {
                        if ( m_noteRegistry.all_of<NoteComponent>(entity) ) {
                            const auto& note =
                                m_noteRegistry.get<NoteComponent>(entity);
                            double noteTime = note.m_timestamp;

                            // 寻找该音符所在的 BPM 区段
                            const TimelineComponent* activeBpm = nullptr;
                            for ( const auto& bpmEv : bpmEvents ) {
                                if ( bpmEv->m_timestamp <= noteTime + 1e-4 ) {
                                    activeBpm = bpmEv;
                                } else {
                                    break;
                                }
                            }

                            if ( activeBpm ) {
                                double bpmVal = activeBpm->m_value;
                                if ( bpmVal <= 0.0 ) bpmVal = 120.0;
                                double beatDuration = 60.0 / bpmVal;

                                // 尝试多个常用分母，找到误差最小且分母最小的拟合
                                static const int denominators[] = {
                                    1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64
                                };
                                int    bestNum   = 0;
                                int    bestDen   = 1;
                                double bestError = 1.0;

                                for ( int den : denominators ) {
                                    double stepDuration = beatDuration / den;
                                    double relative =
                                        noteTime - activeBpm->m_timestamp;
                                    double steps =
                                        std::round(relative / stepDuration);
                                    double fitTime = activeBpm->m_timestamp +
                                                     steps * stepDuration;
                                    double error = std::abs(noteTime - fitTime);

                                    // 如果误差足够小 (小于
                                    // 1ms)，或者显著优于之前的拟合
                                    if ( error < 0.001 ||
                                         error < bestError * 0.5 ) {
                                        bestError = error;
                                        // 归一化分数
                                        int64_t totalSteps =
                                            static_cast<int64_t>(steps);
                                        int beatIndex = totalSteps % den;
                                        if ( beatIndex < 0 ) beatIndex += den;

                                        if ( beatIndex == 0 ) {
                                            bestNum = 1;
                                            bestDen = 1;
                                        } else {
                                            int common =
                                                std::gcd(beatIndex, den);
                                            bestNum = beatIndex / common;
                                            bestDen = den / common;
                                        }

                                        if ( error < 0.0001 )
                                            break;  // 已经非常精确，停止搜索
                                    }
                                }
                                snapshot->hoveredNoteNumerator   = bestNum;
                                snapshot->hoveredNoteDenominator = bestDen;
                                snapshot->hoveredNoteTime        = noteTime;
                            }
                        }
                        break;  // 只处理一个悬停物体
                    }
                }
            }
        }

        // 判定线高度比例计算
        float judgmentLineY =
            camera.viewportHeight * config.visual.judgeline_pos;

        // 获取主视口高度用于预览区比例对齐
        float finalMainHeight =
            camera.viewportHeight;  // 默认为当前视口高度，防止除以 0 或比例错乱
        auto itMainFinal = m_cameras.find("Basic2DCanvas");
        if ( itMainFinal != m_cameras.end() ) {
            finalMainHeight = itMainFinal->second.viewportHeight;
        }

        // 3. 调用 ECS System 针对当前 Camera 生成渲染快照
        // 使用视觉时间 m_visualTime 进行剔除 and 位置映射
        System::NoteRenderSystem::generateSnapshot(m_noteRegistry,
                                                   m_timelineRegistry,
                                                   snapshot,
                                                   cameraId,
                                                   m_visualTime,
                                                   camera.viewportWidth,
                                                   camera.viewportHeight,
                                                   judgmentLineY,
                                                   m_trackCount,
                                                   config,
                                                   finalMainHeight);

        // 4. 生成打击特效 (仅在主画布和预览区显示)
        if ( cameraId == "Basic2DCanvas" || cameraId == "Preview" ) {
            float leftX = camera.viewportWidth * config.visual.trackLayout.left;
            float rightX =
                camera.viewportWidth * config.visual.trackLayout.right;
            float trackAreaW   = rightX - leftX;
            float singleTrackW = trackAreaW / static_cast<float>(m_trackCount);

            // 针对预览区，布局参数略有不同
            if ( cameraId == "Preview" ) {
                leftX        = config.visual.previewConfig.margin.left;
                rightX       = camera.viewportWidth -
                               config.visual.previewConfig.margin.right;
                trackAreaW   = rightX - leftX;
                singleTrackW = trackAreaW / static_cast<float>(m_trackCount);
            }

            m_hitFXSystem.generateSnapshot(snapshot,
                                           m_visualTime,
                                           config,
                                           m_trackCount,
                                           judgmentLineY,
                                           leftX,
                                           singleTrackW);
        }

        // 5. 提交专属快照
        syncBuffer->pushWorkingSnapshot();
    }
}

void BeatmapSession::updateMarqueeSelection(bool forceFullSync)
{
    if ( m_marqueeBoxes.empty() ) return;

    auto mode = m_lastConfig.settings.selectionMode;
    auto view = m_noteRegistry.view<NoteComponent>();

    for ( auto entity : view ) {
        const auto& note = view.get<NoteComponent>(entity);
        if ( note.m_isSubNote ) continue;

        bool isSelectedInAny = false;
        for ( const auto& box : m_marqueeBoxes ) {
            double minTime  = std::min(box.startTime, box.endTime);
            double maxTime  = std::max(box.startTime, box.endTime);
            float  minTrack = std::min(box.startTrack, box.endTrack);
            float  maxTrack = std::max(box.startTrack, box.endTrack);

            bool insideThis = false;
            if ( mode == Config::SelectionMode::Strict ) {
                // 严格模式：物件必须完全在框内
                double noteEnd = note.m_timestamp + note.m_duration;
                float  trackL  = static_cast<float>(note.m_trackIndex);
                float  trackR  = trackL + 1.0f;
                if ( note.m_type == ::MMM::NoteType::FLICK ) {
                    if ( note.m_dtrack > 0 )
                        trackR += note.m_dtrack;
                    else
                        trackL += note.m_dtrack;
                }

                if ( note.m_timestamp >= minTime && noteEnd <= maxTime &&
                     trackL >= minTrack && trackR <= maxTrack ) {
                    insideThis = true;
                }
            } else {
                // 相交模式
                double noteEnd = note.m_timestamp + note.m_duration;
                float  trackL  = static_cast<float>(note.m_trackIndex);
                float  trackR  = trackL + 1.0f;
                if ( note.m_type == ::MMM::NoteType::FLICK ) {
                    if ( note.m_dtrack > 0 )
                        trackR += note.m_dtrack;
                    else
                        trackL += note.m_dtrack;
                }

                bool timeOverlap = std::max(note.m_timestamp, minTime) <=
                                   std::min(noteEnd, maxTime);
                bool trackOverlap =
                    std::max(trackL, minTrack) <= std::min(trackR, maxTrack);
                if ( timeOverlap && trackOverlap ) {
                    insideThis = true;
                }
            }

            if ( insideThis ) {
                isSelectedInAny = true;
                break;
            }
        }

        if ( !m_noteRegistry.all_of<InteractionComponent>(entity) ) {
            m_noteRegistry.emplace<InteractionComponent>(entity);
        }

        auto& ic = m_noteRegistry.get<InteractionComponent>(entity);
        if ( m_marqueeIsAdditive && !forceFullSync ) {
            if ( isSelectedInAny ) {
                ic.isSelected = true;
            }
        } else {
            ic.isSelected = isSelectedInAny;
        }
    }
}

}  // namespace MMM::Logic
