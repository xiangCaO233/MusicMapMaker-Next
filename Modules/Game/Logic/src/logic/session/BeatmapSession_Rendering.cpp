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
#include "logic/session/InteractionController.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <numeric>

namespace MMM::Logic
{

void BeatmapSession::updateECSAndRender(const Config::EditorConfig& config)
{
    // 1. 调用 ECS System 更新全局物理位置 (Logical Transform)
    // 注意：物理位置更新应基于逻辑时间 m_ctx->currentTime
    System::NoteTransformSystem::update(m_ctx->noteRegistry,
                                        m_ctx->timelineRegistry,
                                        m_ctx->currentTime,
                                        config);

    // 筛选出所有 BPM 标记供后续视口处理（磁吸、智能拟合等）
    std::vector<const TimelineComponent*> bpmEvents;
    auto tlView = m_ctx->timelineRegistry.view<const TimelineComponent>();
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
    if ( m_ctx->isSelecting || !m_ctx->marqueeBoxes.empty() ) {
        m_interaction->updateMarqueeSelection();
    }

    // 2. 遍历所有注册的视口 (Camera) 进行独立的视口剔除和坐标映射
    for ( auto& [cameraId, camera] : m_ctx->cameras ) {
        // 从 EditorEngine 获取该 Camera 专属的缓冲
        auto syncBuffer = EditorEngine::instance().getSyncBuffer(cameraId);
        if ( !syncBuffer ) continue;

        RenderSnapshot* snapshot = syncBuffer->getWorkingSnapshot();
        if ( !snapshot ) continue;

        snapshot->clear();

        // 注入该 Camera 特有的 UV 映射到快照
        snapshot->uvMap     = EditorEngine::instance().getAtlasUVMap(cameraId);
        snapshot->isPlaying = m_ctx->isPlaying;
        snapshot->currentTime = m_ctx->visualTime;  // 快照使用视觉平滑时间
        snapshot->totalTime   = Audio::AudioManager::instance().getTotalTime();
        snapshot->snapshotSysTime =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        snapshot->playbackSpeed =
            Audio::AudioManager::instance().getPlaybackSpeed();
        snapshot->hasBeatmap = (m_ctx->currentBeatmap != nullptr);

        if ( m_ctx->currentBeatmap ) {
            auto bgPath =
                m_ctx->currentBeatmap->m_baseMapMetadata.map_path
                    .parent_path() /
                m_ctx->currentBeatmap->m_baseMapMetadata.main_cover_path;
            snapshot->backgroundPath = bgPath.string();
            snapshot->bgSize         = m_ctx->bgSize;
        }

        // --- 注入交互状态 ---
        snapshot->currentTool = m_ctx->currentTool;
        snapshot->isHoveringCanvas =
            m_ctx->isMouseInCanvas && (m_ctx->mouseCameraId == cameraId);

        // 核心修复：预览区的拖拽状态广播
        // 如果预览区正在拖拽，所有视口的渲染快照都需要知道预览区当前的悬停时间点。
        snapshot->isPreviewDragging =
            m_ctx->isDragging && (m_ctx->dragCameraId == "Preview" ||
                                  m_ctx->mouseCameraId == "Preview");
        snapshot->previewHoverTime = m_ctx->previewHoverTime;

        // --- 注入框选状态 ---
        snapshot->isSelecting = m_ctx->isSelecting;
        if ( m_ctx->isSelecting && !m_ctx->marqueeBoxes.empty() ) {
            snapshot->activeSelectionCameraId =
                m_ctx->marqueeBoxes.back().cameraId;
        }

        for ( const auto& box : m_ctx->marqueeBoxes ) {
            RenderSnapshot::MarqueeBoxSnapshot boxSnap;
            boxSnap.startTime  = box.startTime;
            boxSnap.endTime    = box.endTime;
            boxSnap.startTrack = box.startTrack;
            boxSnap.endTrack   = box.endTrack;
            boxSnap.cameraId   = box.cameraId;
            snapshot->marqueeBoxes.push_back(boxSnap);
        }

        if ( snapshot->isHoveringCanvas ) {
            auto* cache =
                m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
            if ( cache ) {
                float judgmentLineY =
                    camera.viewportHeight * config.visual.judgeline_pos;

                double currentAbsY = cache->getAbsY(m_ctx->visualTime);
                double deltaY      = (judgmentLineY - m_ctx->lastMousePos.y);

                float renderScaleY = 1.0f;
                // 核心修复：预览区的坐标是经过压缩的，计算时间时需要除以缩放比例
                if ( cameraId == "Preview" || cameraId == "PreviewCanvas" ) {
                    float previewMainHeight = 1000.0f;
                    auto  itMainPreview = m_ctx->cameras.find("Basic2DCanvas");
                    if ( itMainPreview != m_ctx->cameras.end() ) {
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
                    trackAreaW / static_cast<float>(m_ctx->trackCount);

                int track = static_cast<int>(
                    std::floor((m_ctx->lastMousePos.x - leftX) / singleTrackW));
                snapshot->hoveredTrack =
                    std::clamp(track, 0, m_ctx->trackCount - 1);

                // --- 磁吸拍线时间戳预览 ---
                auto snap = SessionUtils::getSnapResult(snapshot->hoveredTime,
                                                        m_ctx->lastMousePos.y,
                                                        camera,
                                                        config,
                                                        bpmEvents,
                                                        m_ctx->timelineRegistry,
                                                        m_ctx->visualTime,
                                                        m_ctx->cameras);
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
                    snapshot->isPreviewDragging = m_ctx->isDragging;
                    snapshot->previewHoverY     = m_ctx->lastMousePos.y;

                    // 核心逻辑：拖动预览区时，主画布应该渲染拖拽处的内容
                    snapshot->previewHoverTime = snapshot->hoveredTime;
                    m_ctx->previewHoverTime    = snapshot->hoveredTime;
                }

                // --- 智能拟合：计算当前悬停物件的最简分拍 ---
                auto interView =
                    m_ctx->noteRegistry.view<InteractionComponent>();
                for ( auto entity : interView ) {
                    const auto& inter =
                        interView.get<InteractionComponent>(entity);
                    if ( inter.isHovered || inter.isDragging ) {
                        if ( m_ctx->noteRegistry.all_of<NoteComponent>(
                                 entity) ) {
                            const auto& note =
                                m_ctx->noteRegistry.get<NoteComponent>(entity);
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
                                if ( bpmVal <= 0.0 ) {
                                    bpmVal = 120.0;
                                    if ( m_ctx->currentBeatmap &&
                                         m_ctx->currentBeatmap
                                                 ->m_baseMapMetadata
                                                 .preference_bpm > 0.0 ) {
                                        bpmVal = m_ctx->currentBeatmap
                                                     ->m_baseMapMetadata
                                                     .preference_bpm;
                                    }
                                }
                                double beatDuration = 60.0 / bpmVal;

                                // 尝试多个常用分母，找到误差最小且分母最小的拟合
                                static const int denominators[] = {
                                    1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
                                    12, 13, 14, 15, 16, 24, 32, 48, 64, 96, 128
                                };
                                int    bestNum   = 0;
                                int    bestDen   = 1;
                                double bestScore = 1e9;
                                for ( int den : denominators ) {
                                    double stepDuration = beatDuration / den;
                                    double relative =
                                        noteTime - activeBpm->m_timestamp;
                                    double steps =
                                        std::round(relative / stepDuration);
                                    double fitTime = activeBpm->m_timestamp +
                                                     steps * stepDuration;
                                    double error = std::abs(noteTime - fitTime);

                                    // 提取该分母下的分拍位
                                    int64_t totalSteps =
                                        static_cast<int64_t>(steps);
                                    int beatIndex = totalSteps % den;
                                    if ( beatIndex < 0 ) beatIndex += den;

                                    // 计算约分后的最简分母，用于权重计算
                                    int finalNum, finalDen;
                                    if ( beatIndex == 0 ) {
                                        finalNum = 1;
                                        finalDen = 1;
                                    } else {
                                        int common = std::gcd(beatIndex, den);
                                        finalNum   = beatIndex / common;
                                        finalDen   = den / common;
                                    }

                                    // --- 核心评分逻辑 ---
                                    // 目标：平衡“误差”与“分母复杂度”
                                    // 惩罚项：每个分母单位增加 0.2ms
                                    // 的“虚拟误差” 这样 3/5 (即使误差 3ms)
                                    // 也会击败 77/128 (误差 0.5ms) 因为 3/5
                                    // 的分母代价是 5*0.2 = 1ms，总分 4ms 而
                                    // 77/128 的分母代价是 128*0.2
                                    // = 25.6ms，总分 26.1ms
                                    double score =
                                        error + (double)finalDen * 0.0002;

                                    if ( score < bestScore ) {
                                        bestScore = score;
                                        bestNum   = finalNum;
                                        bestDen   = finalDen;
                                    }
                                }
                                snapshot->hoveredNoteNumerator   = bestNum;
                                snapshot->hoveredNoteDenominator = bestDen;
                                snapshot->hoveredNoteTime        = noteTime;

                                // --- 计算物件的详细拍序 (Total Beat Index) ---
                                int64_t totalBeatsPrefix = 0;
                                for ( size_t i = 0; i < bpmEvents.size();
                                      ++i ) {
                                    const auto* bpmEv = bpmEvents[i];
                                    if ( !bpmEv || bpmEv == activeBpm ) break;

                                    double nextTime =
                                        (i + 1 < bpmEvents.size())
                                            ? bpmEvents[i + 1]->m_timestamp
                                            : activeBpm->m_timestamp;
                                    double dur = nextTime - bpmEv->m_timestamp;
                                    if ( dur < 0 ) dur = 0;

                                    double bVal = bpmEv->m_value;
                                    if ( bVal <= 0.0 ) {
                                        bVal = 120.0;
                                        if ( m_ctx->currentBeatmap &&
                                             m_ctx->currentBeatmap
                                                     ->m_baseMapMetadata
                                                     .preference_bpm > 0.0 ) {
                                            bVal = m_ctx->currentBeatmap
                                                       ->m_baseMapMetadata
                                                       .preference_bpm;
                                        }
                                    }
                                    totalBeatsPrefix += static_cast<int64_t>(
                                        std::round(dur / (60.0 / bVal)));
                                }
                                double rel = noteTime - activeBpm->m_timestamp;
                                if ( rel < 0 ) rel = 0;
                                int64_t beatsInActive = static_cast<int64_t>(
                                    std::floor(rel / beatDuration + 1e-6));
                                snapshot->hoveredNoteBeatIndex =
                                    static_cast<int>(totalBeatsPrefix +
                                                     beatsInActive + 1);
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
        auto itMainFinal = m_ctx->cameras.find("Basic2DCanvas");
        if ( itMainFinal != m_ctx->cameras.end() ) {
            finalMainHeight = itMainFinal->second.viewportHeight;
        }

        snapshot->trackCount = m_ctx->trackCount;

        // --- 注入画笔预览状态 ---
        if ( m_ctx->brushState.isActive ) {
            snapshot->brush.isActive = true;
            snapshot->brush.time     = m_ctx->brushState.time;
            snapshot->brush.duration = m_ctx->brushState.duration;
            snapshot->brush.track    = m_ctx->brushState.track;
            snapshot->brush.dtrack   = m_ctx->brushState.dtrack;
            snapshot->brush.type     = m_ctx->brushState.type;
            snapshot->brush.polylineSegments =
                m_ctx->brushState.polylineSegments;
        }

        // --- 注入橡皮擦预览状态 ---
        if ( m_ctx->eraserState.isActive ) {
            snapshot->erasingEntities = m_ctx->eraserState.targetEntities;
        }


        // 3. 调用 ECS System 针对当前 Camera 生成渲染快照
        // 使用视觉时间 m_ctx->visualTime 进行剔除 and 位置映射
        System::NoteRenderSystem::generateSnapshot(m_ctx->noteRegistry,
                                                   m_ctx->timelineRegistry,
                                                   snapshot,
                                                   cameraId,
                                                   m_ctx->visualTime,
                                                   camera.viewportWidth,
                                                   camera.viewportHeight,
                                                   judgmentLineY,
                                                   m_ctx->trackCount,
                                                   config,
                                                   finalMainHeight,
                                                   &m_ctx->hitFXSystem);

        // 5. 提交专属快照
        syncBuffer->pushWorkingSnapshot();
    }
}


}  // namespace MMM::Logic
