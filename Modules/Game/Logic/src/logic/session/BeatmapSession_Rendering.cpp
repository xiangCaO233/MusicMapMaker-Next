#include "audio/AudioManager.h"
#include "config/Utf8Path.h"
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
#include <algorithm>
#include <filesystem>
#include <numeric>

namespace MMM::Logic
{

void BeatmapSession::updateECSAndRender(const Config::EditorConfig& config)
{
    // Rebuild sorted note entities cache if needed
    if ( m_ctx->sortedNoteEntities.empty() || m_ctx->isTransformDirty ) {
        auto noteView = m_ctx->noteRegistry.view<const NoteComponent>();
        m_ctx->sortedNoteEntities.assign(noteView.begin(), noteView.end());
        std::sort(m_ctx->sortedNoteEntities.begin(),
                  m_ctx->sortedNoteEntities.end(),
                  [this](entt::entity a, entt::entity b) {
                      return m_ctx->noteRegistry.get<const NoteComponent>(a)
                                 .m_timestamp <
                             m_ctx->noteRegistry.get<const NoteComponent>(b)
                                 .m_timestamp;
                  });
        m_ctx->sortedNoteMaxEndPrefix.clear();
        m_ctx->sortedNoteMaxEndPrefix.reserve(m_ctx->sortedNoteEntities.size());
        double maxEndTime = 0.0;
        for ( auto entity : m_ctx->sortedNoteEntities ) {
            const auto& note =
                m_ctx->noteRegistry.get<const NoteComponent>(entity);
            double noteEnd = note.m_timestamp + std::max(0.0, note.m_duration);
            if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
                for ( const auto& sub : note.m_subNotes ) {
                    noteEnd = std::max(
                        noteEnd, sub.timestamp + std::max(0.0, sub.duration));
                }
            }
            maxEndTime = std::max(maxEndTime, noteEnd);
            m_ctx->sortedNoteMaxEndPrefix.push_back(maxEndTime);
        }
    }

    m_ctx->noteRegistry.ctx().erase<const std::vector<entt::entity>*>();
    m_ctx->noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(
        &m_ctx->sortedNoteEntities);
    m_ctx->noteRegistry.ctx().erase<const std::vector<double>*>();
    m_ctx->noteRegistry.ctx().emplace<const std::vector<double>*>(
        &m_ctx->sortedNoteMaxEndPrefix);

    // 1. 调用 ECS System 更新全局物理位置 (Logical Transform)
    // 注意：物理位置更新应基于逻辑时间 m_ctx->currentTime
    System::NoteTransformSystem::update(m_ctx->noteRegistry,
                                        m_ctx->timelineRegistry,
                                        m_ctx->currentTime,
                                        config,
                                        m_ctx->isTransformDirty);
    m_ctx->isTransformDirty = false;

    // 0. 更新 BPM 缓存（仅在脏时执行 O(N log N) 操作）
    if ( m_ctx->isBpmEventsDirty ) {
        m_ctx->bpmEvents.clear();
        auto tlView = m_ctx->timelineRegistry.view<const TimelineComponent>();
        for ( auto entity : tlView ) {
            const auto& tl = tlView.get<const TimelineComponent>(entity);
            if ( tl.m_effect == ::MMM::TimingEffect::BPM ) {
                m_ctx->bpmEvents.push_back(&tl);
            }
        }
        std::stable_sort(
            m_ctx->bpmEvents.begin(),
            m_ctx->bpmEvents.end(),
            [](const TimelineComponent* a, const TimelineComponent* b) {
                return a->m_timestamp < b->m_timestamp;
            });
        m_ctx->isBpmEventsDirty = false;
    }

    // 筛选出所有 BPM 标记供后续视口处理（磁轴、智能拟合等）
    const auto& bpmEvents = m_ctx->bpmEvents;

    // 0. 框选区域变化时更新选中状态，避免旧框选框每帧覆盖手动选择。
    if ( m_ctx->isMarqueeSelectionDirty ) {
        m_interaction->updateMarqueeSelection();
    }

    // 2. 遍历所有注册的视口 (Camera) 进行独立的视口剔除和坐标映射
    for ( auto& [cameraId, camera] : m_ctx->cameras ) {
        // 只有活跃 Session 才能往 Preview 和 Timeline 缓冲写入，避免后台
        // Session 覆盖
        if ( (cameraId == "Preview" || cameraId == "Timeline") &&
             EditorEngine::instance().getActiveSession().get() != this ) {
            continue;
        }

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
        snapshot->hasBeatmap        = (m_ctx->currentBeatmap != nullptr);
        snapshot->lastActionMessage = m_ctx->lastActionMessage;

        if ( m_ctx->currentBeatmap ) {
            std::filesystem::path bgPath;
            auto* project = EditorEngine::instance().getCurrentProject();
            if ( project ) {
                bgPath =
                    project->m_projectRoot /
                    m_ctx->currentBeatmap->m_baseMapMetadata.main_cover_path;
            } else {
                bgPath =
                    m_ctx->currentBeatmap->m_baseMapMetadata.map_path
                        .parent_path() /
                    m_ctx->currentBeatmap->m_baseMapMetadata.main_cover_path;
            }
            snapshot->backgroundPath = Config::pathToUtf8(bgPath);
            snapshot->bgSize         = m_ctx->bgSize;
            snapshot->beatmapName =
                m_ctx->currentBeatmap->m_baseMapMetadata.name;
            snapshot->isDirty = m_ctx->actionStack.isDirty();
        }

        // 计算可见时间范围 (基于平滑视觉时间)
        auto* cache = m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
        if ( cache ) {
            float judgmentLineY =
                camera.viewportHeight * config.visual.judgeline_pos;
            double currentAbsY = cache->getAbsY(m_ctx->visualTime);
            // osu! 模式: timelineZoom 已写入 absY 流速，不在此处重复除以 scale
            double scale = snapshot->renderScaleY;
            if ( !SessionUtils::isMainCanvasCameraId(cameraId) &&
                 std::abs(scale) > 0.0001f ) {
                if ( std::abs(scale) < 1e-6 ) scale = 1.0;
            } else if ( SessionUtils::isMainCanvasCameraId(cameraId) ) {
                scale = 1.0;
            }

            snapshot->visibleTimeStart = cache->getTime(
                currentAbsY - (camera.viewportHeight - judgmentLineY) / scale);
            snapshot->visibleTimeEnd =
                cache->getTime(currentAbsY + judgmentLineY / scale);
        }

        // --- 注入交互状态 ---
        snapshot->currentTool = m_ctx->currentTool;
        snapshot->isHoveringCanvas =
            m_ctx->isMouseInCanvas && (m_ctx->mouseCameraId == cameraId);

        // 核心修复：预览区的拖拽状态广播
        // 如果预览区正在拖拽，所有视口的渲染快照都需要知道预览区当前的悬停时间点。
        snapshot->isPreviewDragging =
            m_ctx->isDragging && (m_ctx->dragCameraId == "Preview" ||
                                  m_ctx->mouseCameraId == "Preview" ||
                                  m_ctx->dragCameraId == "AudioWaveform" ||
                                  m_ctx->dragCameraId == "AudioSpectrum");
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
                    const auto* mainCamera =
                        SessionUtils::findMainCanvasCamera(m_ctx->cameras);
                    float previewMainHeight =
                        mainCamera ? mainCamera->viewportHeight : 1000.0f;

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

                if ( cameraId == "Preview" || cameraId == "PreviewCanvas" ) {
                    leftX  = config.visual.previewConfig.margin.left;
                    rightX = camera.viewportWidth -
                             config.visual.previewConfig.margin.right;
                }
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

                // 判断是否在轨道框内
                bool isInsideTrack = (m_ctx->lastMousePos.x >= leftX &&
                                      m_ctx->lastMousePos.x <= rightX);

                if ( snap.isSnapped ) {
                    if ( cameraId == "Timeline" || isInsideTrack ) {
                        snapshot->isSnapped          = true;
                        snapshot->snappedTime        = snap.snappedTime;
                        snapshot->snappedNumerator   = snap.numerator;
                        snapshot->snappedDenominator = snap.denominator;
                    }
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

                auto makeBeatPoint = [&](double time, int32_t track) {
                    HoverBeatPoint point;
                    point.show  = true;
                    point.time  = time;
                    point.track = track;

                    const TimelineComponent* activeBpm = nullptr;
                    for ( const auto& bpmEv : bpmEvents ) {
                        if ( bpmEv->m_timestamp <= time + 1e-4 ) {
                            activeBpm = bpmEv;
                        } else {
                            break;
                        }
                    }
                    if ( !activeBpm ) return point;

                    double bpmVal = activeBpm->m_value;
                    if ( bpmVal <= 0.0 ) {
                        bpmVal = 120.0;
                        if ( m_ctx->currentBeatmap &&
                             m_ctx->currentBeatmap->m_baseMapMetadata
                                     .preference_bpm > 0.0 ) {
                            bpmVal = m_ctx->currentBeatmap->m_baseMapMetadata
                                         .preference_bpm;
                        }
                    }

                    double           beatDuration   = 60.0 / bpmVal;
                    static const int denominators[] = { 1,  2,  3,  4,  5,  6,
                                                        7,  8,  9,  10, 11, 12,
                                                        13, 14, 15, 16, 24, 32,
                                                        48, 64, 96, 128 };
                    int              bestNum        = 0;
                    int              bestDen        = 1;
                    double           bestScore      = 1e9;
                    for ( int den : denominators ) {
                        double stepDuration = beatDuration / den;
                        double relative     = time - activeBpm->m_timestamp;
                        double steps = std::round(relative / stepDuration);
                        double fitTime =
                            activeBpm->m_timestamp + steps * stepDuration;
                        double error = std::abs(time - fitTime);

                        int64_t totalSteps = static_cast<int64_t>(steps);
                        int     beatIndex  = totalSteps % den;
                        if ( beatIndex < 0 ) beatIndex += den;

                        int finalNum = 1;
                        int finalDen = 1;
                        if ( beatIndex != 0 ) {
                            int common = std::gcd(beatIndex, den);
                            finalNum   = beatIndex / common;
                            finalDen   = den / common;
                        }

                        double score = error + (double)finalDen * 0.0002;
                        if ( score < bestScore ) {
                            bestScore = score;
                            bestNum   = finalNum;
                            bestDen   = finalDen;
                        }
                    }

                    int64_t totalBeatsPrefix = 0;
                    for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
                        const auto* bpmEv = bpmEvents[i];
                        if ( !bpmEv || bpmEv == activeBpm ) break;

                        double nextTime = (i + 1 < bpmEvents.size())
                                              ? bpmEvents[i + 1]->m_timestamp
                                              : activeBpm->m_timestamp;
                        double dur      = nextTime - bpmEv->m_timestamp;
                        if ( dur < 0 ) dur = 0;

                        double bVal = bpmEv->m_value;
                        if ( bVal <= 0.0 ) {
                            bVal = 120.0;
                            if ( m_ctx->currentBeatmap &&
                                 m_ctx->currentBeatmap->m_baseMapMetadata
                                         .preference_bpm > 0.0 ) {
                                bVal = m_ctx->currentBeatmap->m_baseMapMetadata
                                           .preference_bpm;
                            }
                        }
                        totalBeatsPrefix += static_cast<int64_t>(
                            std::round(dur / (60.0 / bVal)));
                    }

                    double rel = time - activeBpm->m_timestamp;
                    if ( rel < 0 ) rel = 0;
                    int64_t beatsInActive = static_cast<int64_t>(
                        std::floor(rel / beatDuration + 1e-6));

                    point.beatIndex =
                        static_cast<int>(totalBeatsPrefix + beatsInActive + 1);
                    point.numerator   = bestNum;
                    point.denominator = bestDen;
                    return point;
                };

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
                            auto hoveredPart =
                                static_cast<HoverPart>(inter.hoveredPart);

                            HoverInspectInfo inspect;
                            inspect.show = true;

                            auto setLegacyPoint =
                                [&](const HoverBeatPoint& point) {
                                    if ( !point.show ) return;
                                    snapshot->hoveredNoteNumerator =
                                        point.numerator;
                                    snapshot->hoveredNoteDenominator =
                                        point.denominator;
                                    snapshot->hoveredNoteBeatIndex =
                                        point.beatIndex;
                                    snapshot->hoveredNoteTime  = point.time;
                                    snapshot->hoveredNoteTrack = point.track;
                                };

                            if ( note.m_type == ::MMM::NoteType::POLYLINE &&
                                 inter.hoveredSubIndex >= 0 &&
                                 inter.hoveredSubIndex <
                                     static_cast<int>(
                                         note.m_subNotes.size()) ) {
                                const auto& sub =
                                    note.m_subNotes[inter.hoveredSubIndex];
                                inspect.track = sub.trackIndex;
                                if ( hoveredPart == HoverPart::PolylineNode ) {
                                    inspect.kind =
                                        (inter.hoveredSubIndex == 0)
                                            ? HoverInspectKind::PolylineHead
                                            : HoverInspectKind::PolylineNode;
                                    inspect.body = makeBeatPoint(
                                        sub.timestamp, sub.trackIndex);
                                    inspect.showTrack = true;
                                } else if ( hoveredPart == HoverPart::HoldEnd &&
                                            sub.type ==
                                                ::MMM::NoteType::HOLD ) {
                                    inspect.kind =
                                        HoverInspectKind::PolylineHoldEnd;
                                    inspect.head = makeBeatPoint(
                                        sub.timestamp, sub.trackIndex);
                                    inspect.end = makeBeatPoint(
                                        sub.timestamp + sub.duration,
                                        sub.trackIndex);
                                    inspect.showDuration = true;
                                    inspect.duration     = sub.duration;
                                    inspect.showTrack    = true;
                                } else if ( hoveredPart ==
                                                HoverPart::FlickArrow &&
                                            sub.type ==
                                                ::MMM::NoteType::FLICK ) {
                                    inspect.kind =
                                        HoverInspectKind::PolylineFlickEnd;
                                    inspect.end = makeBeatPoint(
                                        sub.timestamp,
                                        sub.trackIndex + sub.dtrack);
                                    inspect.showDtrack = true;
                                    inspect.dtrack     = sub.dtrack;
                                    inspect.showTrack  = true;
                                    inspect.track = sub.trackIndex + sub.dtrack;
                                } else if ( sub.type ==
                                            ::MMM::NoteType::FLICK ) {
                                    inspect.kind =
                                        HoverInspectKind::PolylineFlickBody;
                                    inspect.body = makeBeatPoint(
                                        sub.timestamp, sub.trackIndex);
                                    inspect.showDtrack = true;
                                    inspect.dtrack     = sub.dtrack;
                                } else {
                                    inspect.kind =
                                        HoverInspectKind::PolylineHoldBody;
                                    inspect.showDuration = true;
                                    inspect.duration     = sub.duration;
                                    inspect.showTrack    = true;
                                }
                            } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
                                inspect.duration = note.m_duration;
                                inspect.track    = note.m_trackIndex;
                                if ( hoveredPart == HoverPart::HoldEnd ) {
                                    inspect.kind = HoverInspectKind::HoldEnd;
                                    inspect.head = makeBeatPoint(
                                        note.m_timestamp, note.m_trackIndex);
                                    inspect.end = makeBeatPoint(
                                        note.m_timestamp + note.m_duration,
                                        note.m_trackIndex);
                                } else if ( hoveredPart ==
                                            HoverPart::HoldBody ) {
                                    inspect.kind = HoverInspectKind::HoldBody;
                                } else {
                                    inspect.kind = HoverInspectKind::HoldHead;
                                    inspect.head = makeBeatPoint(
                                        note.m_timestamp, note.m_trackIndex);
                                }
                                inspect.showDuration = true;
                                inspect.showTrack    = true;
                            } else if ( note.m_type ==
                                        ::MMM::NoteType::FLICK ) {
                                inspect.dtrack = note.m_dtrack;
                                if ( hoveredPart == HoverPart::FlickArrow ) {
                                    inspect.kind = HoverInspectKind::FlickEnd;
                                    inspect.end  = makeBeatPoint(
                                        note.m_timestamp,
                                        note.m_trackIndex + note.m_dtrack);
                                    inspect.track =
                                        note.m_trackIndex + note.m_dtrack;
                                    inspect.showTrack = true;
                                } else if ( hoveredPart ==
                                            HoverPart::HoldBody ) {
                                    inspect.kind = HoverInspectKind::FlickBody;
                                    inspect.body = makeBeatPoint(
                                        note.m_timestamp, note.m_trackIndex);
                                } else {
                                    inspect.kind = HoverInspectKind::FlickHead;
                                    inspect.head = makeBeatPoint(
                                        note.m_timestamp, note.m_trackIndex);
                                    inspect.track     = note.m_trackIndex;
                                    inspect.showTrack = true;
                                }
                                inspect.showDtrack = true;
                            } else {
                                inspect.kind = HoverInspectKind::Note;
                                inspect.head = makeBeatPoint(note.m_timestamp,
                                                             note.m_trackIndex);
                                inspect.track     = note.m_trackIndex;
                                inspect.showTrack = true;
                            }

                            snapshot->hoverInspect = inspect;
                            if ( inspect.head.show ) {
                                setLegacyPoint(inspect.head);
                            } else if ( inspect.body.show ) {
                                setLegacyPoint(inspect.body);
                            } else if ( inspect.end.show ) {
                                setLegacyPoint(inspect.end);
                            }
                        }
                        break;  // 只处理一个悬停物体
                    }
                }
            }
        }

        // 非预览区拖拽时（AudioWaveform/AudioSpectrum），从 previewHoverTime
        // 反算 previewHoverY
        if ( cameraId == "Preview" && snapshot->isPreviewDragging &&
             !snapshot->isHoveringCanvas ) {
            auto* cache =
                m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
            if ( cache ) {
                float judgmentLineY =
                    camera.viewportHeight * config.visual.judgeline_pos;
                double currentAbsY = cache->getAbsY(m_ctx->visualTime);
                double targetAbsY  = cache->getAbsY(snapshot->previewHoverTime);

                const auto* mainCamera =
                    SessionUtils::findMainCanvasCamera(m_ctx->cameras);
                float mainViewportHeight =
                    mainCamera ? mainCamera->viewportHeight : 1000.0f;

                float mainEffectiveH = (config.visual.trackLayout.bottom -
                                        config.visual.trackLayout.top) *
                                       mainViewportHeight;
                float previewDrawH =
                    camera.viewportHeight -
                    (config.visual.previewConfig.margin.top +
                     config.visual.previewConfig.margin.bottom);
                float renderScaleY =
                    previewDrawH /
                    (mainEffectiveH * config.visual.previewConfig.areaRatio);

                if ( std::abs(renderScaleY) > 0.0001f ) {
                    snapshot->previewHoverY =
                        judgmentLineY -
                        static_cast<float>((targetAbsY - currentAbsY) *
                                           renderScaleY);
                }
            }
        }

        // 判定线高度比例计算
        float judgmentLineY =
            camera.viewportHeight * config.visual.judgeline_pos;

        // 获取主视口高度用于预览区比例对齐
        float finalMainHeight =
            camera.viewportHeight;  // 默认为当前视口高度，防止除以 0 或比例错乱
        const auto* mainCameraFinal =
            SessionUtils::findMainCanvasCamera(m_ctx->cameras);
        if ( mainCameraFinal ) {
            finalMainHeight = mainCameraFinal->viewportHeight;
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
            snapshot->erasingSubIndex = -1;

            // Shift 模式下保持 erasingSubIndex = -1，使整个 Polyline 标红
            if ( !m_ctx->eraserState.isShiftDown ) {
                // 非 Shift：悬停在 Polyline 的任意子物件时，允许局部高亮红色
                if ( m_ctx->hoveredEntity != entt::null &&
                     m_ctx->noteRegistry.all_of<NoteComponent>(
                         m_ctx->hoveredEntity) ) {
                    const auto& nc = m_ctx->noteRegistry.get<NoteComponent>(
                        m_ctx->hoveredEntity);
                    if ( nc.m_type == ::MMM::NoteType::POLYLINE &&
                         !nc.m_subNotes.empty() ) {
                        if ( m_ctx->hoveredSubIndex >= 0 &&
                             m_ctx->hoveredSubIndex <
                                 static_cast<int>(nc.m_subNotes.size()) ) {
                            snapshot->erasingSubIndex = m_ctx->hoveredSubIndex;
                        }
                    }
                }
            }
        }


        // 3. 调用 ECS System 针对当前 Camera 生成渲染快照
        // 使用视觉时间 m_ctx->visualTime 进行剔除 and 位置映射
        System::NoteRenderSystem::generateSnapshot(m_ctx->noteRegistry,
                                                   m_ctx->timelineRegistry,
                                                   bpmEvents,
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
