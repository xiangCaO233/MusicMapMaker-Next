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
#include <cmath>
#include <filesystem>
#include <numeric>

namespace MMM::Logic
{

namespace
{
/// @brief 谱面状态栏统计缓存。
struct BeatmapStatusStats {
    /// @brief 当前谱面的可计数物件数量。
    size_t noteCount{ 0 };

    /// @brief 当前谱面的最大连击数。
    size_t maxCombo{ 0 };
};

/// @brief 判断视图是否属于播放态可背压的辅助画布。
/// @param cameraId 当前画布 ID。
/// @return Preview 或 Timeline 返回 true。
/// @warning 逻辑热路径：只做固定字符串比较。
bool isPlaybackSecondaryCameraId(const std::string& cameraId)
{
    return cameraId == "Preview" || cameraId == "Timeline";
}

/// @brief 计算 Hold 区间内的 1/4 拍连击增量。
/// @param startTime Hold 起始时间。
/// @param endTime Hold 结束时间。
/// @param beatmap 当前谱面数据。
/// @return 区间内按 BPM 分段累计的连击增量。
/// @warning 逻辑热路径低频分支：仅在音符/时间线脏标记触发统计重算时执行；禁止每
/// update 无条件调用。
size_t calculateIntervalCombos(double startTime, double endTime,
                               const ::MMM::BeatMap* beatmap)
{
    if ( !beatmap || endTime <= startTime ) {
        return 0;
    }

    double totalQuarterBeats = 0.0;
    double currTime          = startTime;

    double currentBpm = beatmap->m_baseMapMetadata.preference_bpm;
    if ( currentBpm <= 0.0 ) currentBpm = 120.0;

    size_t nextTimingIdx = 0;
    for ( size_t i = 0; i < beatmap->m_timings.size(); ++i ) {
        const auto& timing = beatmap->m_timings[i];
        if ( timing.m_timingEffect == ::MMM::TimingEffect::BPM ) {
            if ( timing.m_timestamp <= startTime ) {
                currentBpm = timing.m_bpm;
            } else {
                nextTimingIdx = i;
                break;
            }
        }
    }

    while ( currTime < endTime ) {
        double nextEventTime = endTime;
        double nextBpm       = currentBpm;
        size_t foundIdx      = beatmap->m_timings.size();

        for ( size_t i = nextTimingIdx; i < beatmap->m_timings.size(); ++i ) {
            const auto& timing = beatmap->m_timings[i];
            if ( timing.m_timingEffect == ::MMM::TimingEffect::BPM &&
                 timing.m_timestamp > currTime ) {
                if ( timing.m_timestamp < endTime ) {
                    nextEventTime = timing.m_timestamp;
                    nextBpm       = timing.m_bpm;
                    foundIdx      = i + 1;
                }
                break;
            }
        }

        double dt = nextEventTime - currTime;
        totalQuarterBeats += dt * (currentBpm / 15.0);

        currTime   = nextEventTime;
        currentBpm = nextBpm;
        if ( foundIdx < beatmap->m_timings.size() ) {
            nextTimingIdx = foundIdx;
        }
    }

    double tolerance = 0.003 * (currentBpm / 15.0);
    return static_cast<size_t>(std::floor(totalQuarterBeats + tolerance));
}

/// @brief 将单个音符组件累计到状态栏统计。
/// @param note 当前音符组件。
/// @param beatmap 当前谱面数据。
/// @param stats 待更新的统计缓存。
void accumulateNoteStats(const NoteComponent&  note,
                         const ::MMM::BeatMap* beatmap,
                         BeatmapStatusStats&   stats)
{
    if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
        for ( const auto& sub : note.m_subNotes ) {
            if ( sub.type == ::MMM::NoteType::NOTE ||
                 sub.type == ::MMM::NoteType::HOLD ||
                 sub.type == ::MMM::NoteType::FLICK ) {
                ++stats.noteCount;
            }
        }
    } else if ( !note.m_isSubNote ) {
        if ( note.m_type == ::MMM::NoteType::NOTE ||
             note.m_type == ::MMM::NoteType::HOLD ||
             note.m_type == ::MMM::NoteType::FLICK ) {
            ++stats.noteCount;
        }
    }

    if ( note.m_isSubNote ) {
        return;
    }

    if ( note.m_type == ::MMM::NoteType::NOTE ||
         note.m_type == ::MMM::NoteType::FLICK ) {
        ++stats.maxCombo;
    } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
        ++stats.maxCombo;
        stats.maxCombo += calculateIntervalCombos(
            note.m_timestamp, note.m_timestamp + note.m_duration, beatmap);
    } else if ( note.m_type == ::MMM::NoteType::POLYLINE &&
                !note.m_subNotes.empty() ) {
        ++stats.maxCombo;
        if ( note.m_subNotes[0].type == ::MMM::NoteType::HOLD ) {
            stats.maxCombo += calculateIntervalCombos(
                note.m_subNotes[0].timestamp,
                note.m_subNotes[0].timestamp + note.m_subNotes[0].duration,
                beatmap);
        }
        for ( size_t i = 1; i < note.m_subNotes.size(); ++i ) {
            const auto& sub = note.m_subNotes[i];
            if ( sub.type == ::MMM::NoteType::FLICK ) {
                ++stats.maxCombo;
            } else if ( sub.type == ::MMM::NoteType::HOLD ) {
                stats.maxCombo += calculateIntervalCombos(
                    sub.timestamp, sub.timestamp + sub.duration, beatmap);
            }
        }
    }
}

/// @brief 计算预览区相对主画布的纵向渲染缩放倍率。
/// @param ctx 当前会话上下文。
/// @param previewCamera 预览区相机信息。
/// @param config 当前编辑器配置。
/// @return 可用的预览区纵向缩放倍率；输入尺寸无效时返回 0。
/// @warning 逻辑/渲染热路径：每个 Session update 的预览相关分支调用；只允许
/// 常量时间计算，禁止 ECS 遍历、文件系统访问和阻塞操作。
float calculatePreviewRenderScaleY(const SessionContext&       ctx,
                                   const CameraInfo&           previewCamera,
                                   const Config::EditorConfig& config)
{
    const auto* mainCamera = SessionUtils::findMainCanvasCamera(ctx.cameras);
    float       mainViewportHeight =
        mainCamera ? mainCamera->viewportHeight : 1000.0f;
    float mainEffectiveH =
        (config.visual.trackLayout.bottom - config.visual.trackLayout.top) *
        mainViewportHeight;
    float previewDrawH = previewCamera.viewportHeight -
                         (config.visual.previewConfig.margin.top +
                          config.visual.previewConfig.margin.bottom);
    float areaRatio    = config.visual.previewConfig.areaRatio;

    if ( mainEffectiveH <= 0.0001f || previewDrawH <= 0.0001f ||
         areaRatio <= 0.0001f ) {
        return 0.0f;
    }

    return previewDrawH / (mainEffectiveH * areaRatio);
}

/// @brief 将当前动画缩放比例同步到 ScrollCache。
/// @param ctx 当前会话上下文。
/// @param config 当前编辑器配置。
/// @warning 逻辑/渲染热路径：每个 Session update 调用；只做常量级查找和赋值。
void syncScrollCacheAnimatedZoom(SessionContext&             ctx,
                                 const Config::EditorConfig& config)
{
    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) {
        return;
    }

    double targetZoom = static_cast<double>(config.visual.timelineZoom);
    if ( !std::isfinite(targetZoom) || targetZoom <= 1e-9 ) {
        targetZoom = 1.0;
    }

    double animateZoom = static_cast<double>(ctx.animatedTimelineZoom);
    if ( !std::isfinite(animateZoom) || animateZoom <= 1e-9 ) {
        animateZoom = targetZoom;
    }

    cache->setAnimatedZoomScale(animateZoom / targetZoom);
}

/// @brief 在生成视口快照前同步预览拖拽目标时间。
/// @param ctx 当前会话上下文。
/// @param config 当前编辑器配置。
/// @warning 逻辑/渲染热路径：每个 Session update 调用；只读取预览相机、
/// ScrollCache 和最后鼠标坐标，禁止引入 ECS 遍历或共享所有权复制。
void syncPreviewDragHoverTime(SessionContext&             ctx,
                              const Config::EditorConfig& config)
{
    if ( !ctx.isDragging ||
         (ctx.dragCameraId != "Preview" && ctx.mouseCameraId != "Preview") ) {
        return;
    }

    auto cameraIt = ctx.cameras.find("Preview");
    if ( cameraIt == ctx.cameras.end() ) {
        return;
    }

    auto* cache = ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) {
        return;
    }

    const auto& previewCamera = cameraIt->second;
    float       renderScaleY =
        calculatePreviewRenderScaleY(ctx, previewCamera, config);
    if ( std::abs(renderScaleY) <= 0.0001f ) {
        return;
    }

    float judgmentLineY =
        previewCamera.viewportHeight * config.visual.judgeline_pos;
    double currentAbsY   = cache->getAbsY(ctx.animateTime);
    double deltaY        = (judgmentLineY - ctx.lastMousePos.y) /
                           static_cast<double>(renderScaleY);
    ctx.previewHoverTime = cache->getTime(currentAbsY + deltaY);
}
}  // namespace

/// @brief 更新 ECS 状态并为当前 Session 的视口生成渲染快照。
/// @warning 逻辑/渲染热路径：每个 Session update 执行；后台 Session
/// 不生成拾取/悬浮交互数据。
void BeatmapSession::updateECSAndRender(const Config::EditorConfig& config,
                                        bool isActiveSession)
{
    auto rebuildNotePrefixAndStats = [this](bool rebuildStats) {
        m_ctx->sortedNoteMaxEndPrefix.clear();
        m_ctx->sortedNoteMaxEndPrefix.reserve(m_ctx->sortedNoteEntities.size());

        BeatmapStatusStats stats;
        const auto*        beatmap    = m_ctx->currentBeatmap.get();
        double             maxEndTime = 0.0;
        for ( auto entity : m_ctx->sortedNoteEntities ) {
            if ( !m_ctx->noteRegistry.valid(entity) ||
                 !m_ctx->noteRegistry.all_of<NoteComponent>(entity) ) {
                continue;
            }

            const auto& note =
                m_ctx->noteRegistry.get<const NoteComponent>(entity);
            if ( rebuildStats ) {
                accumulateNoteStats(note, beatmap, stats);
            }

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

        if ( rebuildStats ) {
            m_ctx->noteCount        = stats.noteCount;
            m_ctx->maxCombo         = stats.maxCombo;
            m_ctx->isNoteStatsDirty = false;
        }
    };

    // Rebuild sorted note entities cache if needed
    if ( m_ctx->isNoteOrderDirty ) {
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
        rebuildNotePrefixAndStats(!m_ctx->m_needsNotesSync);
        ++m_ctx->noteVisibilityIndexRevision;
        m_ctx->isNoteOrderDirty = false;
        m_ctx->isNotePruneDirty = false;
    } else if ( m_ctx->isNotePruneDirty ) {
        auto isEntityInvalid = [this](entt::entity entity) {
            return !m_ctx->noteRegistry.valid(entity) ||
                   !m_ctx->noteRegistry.all_of<NoteComponent>(entity);
        };
        m_ctx->sortedNoteEntities.erase(
            std::remove_if(m_ctx->sortedNoteEntities.begin(),
                           m_ctx->sortedNoteEntities.end(),
                           isEntityInvalid),
            m_ctx->sortedNoteEntities.end());
        rebuildNotePrefixAndStats(!m_ctx->m_needsNotesSync);
        ++m_ctx->noteVisibilityIndexRevision;
        m_ctx->isNotePruneDirty = false;
    } else if ( m_ctx->isNoteStatsDirty && !m_ctx->m_needsNotesSync ) {
        rebuildNotePrefixAndStats(true);
    }

    if ( auto** sortedEntitiesPtr =
             m_ctx->noteRegistry.ctx()
                 .find<const std::vector<entt::entity>*>() ) {
        *sortedEntitiesPtr = &m_ctx->sortedNoteEntities;
    } else {
        m_ctx->noteRegistry.ctx().emplace<const std::vector<entt::entity>*>(
            &m_ctx->sortedNoteEntities);
    }
    if ( auto** maxEndPrefixPtr =
             m_ctx->noteRegistry.ctx().find<const std::vector<double>*>() ) {
        *maxEndPrefixPtr = &m_ctx->sortedNoteMaxEndPrefix;
    } else {
        m_ctx->noteRegistry.ctx().emplace<const std::vector<double>*>(
            &m_ctx->sortedNoteMaxEndPrefix);
    }
    if ( auto** revisionPtr =
             m_ctx->noteRegistry.ctx().find<const std::uint64_t*>() ) {
        *revisionPtr = &m_ctx->noteVisibilityIndexRevision;
    } else {
        m_ctx->noteRegistry.ctx().emplace<const std::uint64_t*>(
            &m_ctx->noteVisibilityIndexRevision);
    }

    syncScrollCacheAnimatedZoom(*m_ctx, config);

    // 1. 调用 ECS System 更新全局物理位置 (Logical Transform)
    // 注意：物理位置更新应基于逻辑时间 m_ctx->currentTime
    System::NoteTransformSystem::update(m_ctx->noteRegistry,
                                        m_ctx->timelineRegistry,
                                        m_ctx->currentTime,
                                        config,
                                        m_ctx->currentBeatmap.get(),
                                        m_ctx->isTransformDirty);
    m_ctx->isTransformDirty = false;

    // 0. 更新 BPM 缓存（仅在脏时执行 O(N log N) 操作）
    SessionUtils::ensureBpmEvents(*m_ctx);

    // 筛选出所有 BPM 标记供后续视口处理（磁轴、智能拟合等）
    const auto& bpmEvents = m_ctx->bpmEvents;

    // 0. 框选区域变化时更新选中状态，避免旧框选框每帧覆盖手动选择。
    if ( m_ctx->isMarqueeSelectionDirty ) {
        m_interaction->updateMarqueeSelection();
    }

    syncPreviewDragHoverTime(*m_ctx, config);

    auto&        engine = EditorEngine::instance();
    const double secondaryCameraSnapshotMinInterval =
        engine.adaptiveRenderSnapshotMinInterval(config, true);
    const bool snapshotIsPlaying =
        m_ctx->isPlaying || m_ctx->isMainAudioSyncFollower;
    const double snapshotTotalTime =
        SessionUtils::getEffectiveTotalTimeSeconds(*m_ctx);
    const double snapshotSysTime =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const double snapshotPlaybackSpeed =
        Audio::AudioManager::instance().getPlaybackSpeed();

    const bool  hasBeatmap = (m_ctx->currentBeatmap != nullptr);
    std::string snapshotBackgroundPath;
    std::string snapshotBeatmapPathKey;
    std::string snapshotBeatmapName;
    bool        snapshotIsDirty     = false;
    double      snapshotFallbackBpm = 120.0;
    if ( m_ctx->currentBeatmap ) {
        const auto& metadata = m_ctx->currentBeatmap->m_baseMapMetadata;
        if ( metadata.preference_bpm > 0.0 &&
             std::isfinite(metadata.preference_bpm) ) {
            snapshotFallbackBpm = metadata.preference_bpm;
        }

        std::filesystem::path bgPath;
        auto*                 project = engine.getCurrentProject();
        if ( project ) {
            bgPath = project->m_projectRoot / metadata.main_cover_path;
        } else {
            bgPath = metadata.map_path.parent_path() / metadata.main_cover_path;
        }
        snapshotBackgroundPath = Config::pathToUtf8(bgPath);
        snapshotBeatmapPathKey = Config::pathToUtf8(metadata.map_path);
        snapshotBeatmapName    = metadata.name;
        snapshotIsDirty        = m_ctx->actionStack.isDirty();
    }

    // 2. 遍历所有注册的视口 (Camera) 进行独立的视口剔除和坐标映射
    for ( auto& [cameraId, camera] : m_ctx->cameras ) {
        // 只有活跃 Session 才能往 Preview 和 Timeline 缓冲写入，避免后台
        // Session 覆盖
        if ( (cameraId == "Preview" || cameraId == "Timeline") &&
             !isActiveSession ) {
            continue;
        }

        const bool isSecondaryPlaybackCamera =
            snapshotIsPlaying && isPlaybackSecondaryCameraId(cameraId);
        const bool isPreviewExternalDrag =
            cameraId == "Preview" && (m_ctx->dragCameraId == "AudioWaveform" ||
                                      m_ctx->dragCameraId == "AudioSpectrum" ||
                                      m_ctx->mouseCameraId == "AudioWaveform" ||
                                      m_ctx->mouseCameraId == "AudioSpectrum");
        const bool isCameraInteractionActive =
            isActiveSession &&
            (m_ctx->mouseCameraId == cameraId ||
             m_ctx->dragCameraId == cameraId || isPreviewExternalDrag ||
             m_ctx->isSelecting || m_ctx->brushState.isActive ||
             m_ctx->eraserState.isActive);
        if ( isSecondaryPlaybackCamera && !isCameraInteractionActive ) {
            auto& lastCameraSnapshotTime =
                m_ctx->lastCameraSnapshotTimes[cameraId];
            if ( lastCameraSnapshotTime > 0.0 &&
                 snapshotSysTime - lastCameraSnapshotTime <
                     secondaryCameraSnapshotMinInterval ) {
                continue;
            }
            lastCameraSnapshotTime = snapshotSysTime;
        } else {
            m_ctx->lastCameraSnapshotTimes[cameraId] = snapshotSysTime;
        }

        // 从 Session 本地缓存获取该 Camera 专属的同步缓冲，避免每帧查注册表锁。
        auto& syncBuffer = m_ctx->syncBuffers[cameraId];
        if ( !syncBuffer ) {
            syncBuffer = engine.getSyncBuffer(cameraId);
        }
        if ( !syncBuffer ) continue;

        RenderSnapshot* snapshot = syncBuffer->getWorkingSnapshot();
        if ( !snapshot ) continue;

        snapshot->clear();

        // 注入该 Camera 特有的 UV 映射到快照
        engine.updateSnapshotAtlasUVMap(
            cameraId, snapshot->uvMap, snapshot->atlasUvRevision);
        snapshot->isPlaying         = snapshotIsPlaying;
        snapshot->currentTime       = m_ctx->animateTime;  // 快照使用动画时间
        snapshot->totalTime         = snapshotTotalTime;
        snapshot->snapshotSysTime   = snapshotSysTime;
        snapshot->playbackSpeed     = snapshotPlaybackSpeed;
        snapshot->fallbackBpm       = snapshotFallbackBpm;
        snapshot->hasBeatmap        = hasBeatmap;
        snapshot->lastActionMessage = m_ctx->lastActionMessage;

        if ( hasBeatmap ) {
            snapshot->backgroundPath = snapshotBackgroundPath;
            snapshot->bgSize         = m_ctx->bgSize;
            snapshot->beatmapPathKey = snapshotBeatmapPathKey;
            snapshot->beatmapName    = snapshotBeatmapName;
            snapshot->isDirty        = snapshotIsDirty;
        }

        // 计算可见时间范围 (基于动画时间)
        auto* cache = m_ctx->timelineRegistry.ctx().find<System::ScrollCache>();
        if ( cache ) {
            float judgmentLineY =
                camera.viewportHeight * config.visual.judgeline_pos;
            double currentAbsY = cache->getAbsY(m_ctx->animateTime);
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
        snapshot->currentTool        = m_ctx->currentTool;
        snapshot->acceptsInteraction = isActiveSession;
        snapshot->noteCount          = m_ctx->noteCount;
        snapshot->maxCombo           = m_ctx->maxCombo;
        snapshot->isHoveringCanvas   = isActiveSession &&
                                       m_ctx->isMouseInCanvas &&
                                       (m_ctx->mouseCameraId == cameraId);

        // 核心修复：预览区的拖拽状态广播
        // 如果预览区正在拖拽，所有视口的渲染快照都需要知道预览区当前的悬停时间点。
        snapshot->isPreviewDragging = isActiveSession && m_ctx->isDragging &&
                                      (m_ctx->dragCameraId == "Preview" ||
                                       m_ctx->mouseCameraId == "Preview" ||
                                       m_ctx->dragCameraId == "AudioWaveform" ||
                                       m_ctx->dragCameraId == "AudioSpectrum");
        snapshot->previewHoverTime  = m_ctx->previewHoverTime;

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

                double currentAbsY = cache->getAbsY(m_ctx->animateTime);
                double deltaY      = (judgmentLineY - m_ctx->lastMousePos.y);

                float renderScaleY = 1.0f;
                // 核心修复：预览区的坐标是经过压缩的，计算时间时需要除以缩放比例
                if ( cameraId == "Preview" || cameraId == "PreviewCanvas" ) {
                    renderScaleY =
                        calculatePreviewRenderScaleY(*m_ctx, camera, config);
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
                                                        m_ctx->animateTime,
                                                        m_ctx->cameras,
                                                        snapshotFallbackBpm);

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
                    if ( !activeBpm && !bpmEvents.empty() &&
                         time < bpmEvents.front()->m_timestamp ) {
                        activeBpm = bpmEvents.front();
                    }
                    if ( !activeBpm ) return point;
                    bool isBeforeFirstBpm = !bpmEvents.empty() &&
                                            activeBpm == bpmEvents.front() &&
                                            time < activeBpm->m_timestamp;

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

                    double  rel           = time - activeBpm->m_timestamp;
                    int64_t beatsInActive = static_cast<int64_t>(
                        std::floor(rel / beatDuration + 1e-6));

                    if ( isBeforeFirstBpm && bestNum == 1 && bestDen == 1 ) {
                        bestNum = 0;
                    }
                    point.beatIndex = isBeforeFirstBpm
                                          ? static_cast<int>(beatsInActive)
                                          : static_cast<int>(totalBeatsPrefix +
                                                             beatsInActive + 1);
                    point.numerator = bestNum;
                    point.denominator = bestDen;
                    return point;
                };

                // --- 智能拟合：计算当前悬停物件的最简分拍 ---
                const entt::entity inspectEntity =
                    (m_ctx->hoveredEntity != entt::null) ? m_ctx->hoveredEntity
                                                         : m_ctx->draggedEntity;
                const bool useDragState = m_ctx->isDragging &&
                                          inspectEntity != entt::null &&
                                          inspectEntity == m_ctx->draggedEntity;
                const auto* inter =
                    (inspectEntity != entt::null &&
                     m_ctx->noteRegistry.valid(inspectEntity))
                        ? m_ctx->noteRegistry
                              .try_get<const InteractionComponent>(
                                  inspectEntity)
                        : nullptr;
                const bool shouldInspect =
                    useDragState ||
                    (inter && (inter->isHovered || inter->isDragging));
                const auto* inspectNote =
                    shouldInspect
                        ? m_ctx->noteRegistry.try_get<const NoteComponent>(
                              inspectEntity)
                        : nullptr;
                if ( inspectNote ) {
                    const auto& note = *inspectNote;
                    const auto  hoveredPart =
                        useDragState
                            ? m_ctx->draggedPart
                            : static_cast<HoverPart>(inter->hoveredPart);
                    const int32_t hoveredSubIndex =
                        useDragState ? m_ctx->draggedSubIndex
                                     : inter->hoveredSubIndex;

                    HoverInspectInfo inspect;
                    inspect.show = true;

                    auto setLegacyPoint = [&](const HoverBeatPoint& point) {
                        if ( !point.show ) return;
                        snapshot->hoveredNoteNumerator   = point.numerator;
                        snapshot->hoveredNoteDenominator = point.denominator;
                        snapshot->hoveredNoteBeatIndex   = point.beatIndex;
                        snapshot->hoveredNoteTime        = point.time;
                        snapshot->hoveredNoteTrack       = point.track;
                    };

                    if ( note.m_type == ::MMM::NoteType::POLYLINE &&
                         hoveredSubIndex >= 0 &&
                         hoveredSubIndex <
                             static_cast<int>(note.m_subNotes.size()) ) {
                        const auto& sub = note.m_subNotes[hoveredSubIndex];
                        inspect.track   = sub.trackIndex;
                        if ( hoveredPart == HoverPart::PolylineNode ) {
                            inspect.kind = (hoveredSubIndex == 0)
                                               ? HoverInspectKind::PolylineHead
                                               : HoverInspectKind::PolylineNode;
                            inspect.body =
                                makeBeatPoint(sub.timestamp, sub.trackIndex);
                            inspect.showTrack = true;
                        } else if ( hoveredPart == HoverPart::HoldEnd &&
                                    sub.type == ::MMM::NoteType::HOLD ) {
                            inspect.kind = HoverInspectKind::PolylineHoldEnd;
                            inspect.head =
                                makeBeatPoint(sub.timestamp, sub.trackIndex);
                            inspect.end = makeBeatPoint(
                                sub.timestamp + sub.duration, sub.trackIndex);
                            inspect.showDuration = true;
                            inspect.duration     = sub.duration;
                            inspect.showTrack    = true;
                        } else if ( hoveredPart == HoverPart::FlickArrow &&
                                    sub.type == ::MMM::NoteType::FLICK ) {
                            inspect.kind = HoverInspectKind::PolylineFlickEnd;
                            inspect.end  = makeBeatPoint(
                                sub.timestamp, sub.trackIndex + sub.dtrack);
                            inspect.showDtrack = true;
                            inspect.dtrack     = sub.dtrack;
                            inspect.showTrack  = true;
                            inspect.track      = sub.trackIndex + sub.dtrack;
                        } else if ( sub.type == ::MMM::NoteType::FLICK ) {
                            inspect.kind = HoverInspectKind::PolylineFlickBody;
                            inspect.body =
                                makeBeatPoint(sub.timestamp, sub.trackIndex);
                            inspect.showDtrack = true;
                            inspect.dtrack     = sub.dtrack;
                        } else {
                            inspect.kind = HoverInspectKind::PolylineHoldBody;
                            inspect.showDuration = true;
                            inspect.duration     = sub.duration;
                            inspect.showTrack    = true;
                        }
                    } else if ( note.m_type == ::MMM::NoteType::HOLD ) {
                        inspect.duration = note.m_duration;
                        inspect.track    = note.m_trackIndex;
                        if ( hoveredPart == HoverPart::HoldEnd ) {
                            inspect.kind = HoverInspectKind::HoldEnd;
                            inspect.head = makeBeatPoint(note.m_timestamp,
                                                         note.m_trackIndex);
                            inspect.end  = makeBeatPoint(
                                note.m_timestamp + note.m_duration,
                                note.m_trackIndex);
                        } else if ( hoveredPart == HoverPart::HoldBody ) {
                            inspect.kind = HoverInspectKind::HoldBody;
                        } else {
                            inspect.kind = HoverInspectKind::HoldHead;
                            inspect.head = makeBeatPoint(note.m_timestamp,
                                                         note.m_trackIndex);
                        }
                        inspect.showDuration = true;
                        inspect.showTrack    = true;
                    } else if ( note.m_type == ::MMM::NoteType::FLICK ) {
                        inspect.dtrack = note.m_dtrack;
                        if ( hoveredPart == HoverPart::FlickArrow ) {
                            inspect.kind = HoverInspectKind::FlickEnd;
                            inspect.end  = makeBeatPoint(
                                note.m_timestamp,
                                note.m_trackIndex + note.m_dtrack);
                            inspect.track = note.m_trackIndex + note.m_dtrack;
                            inspect.showTrack = true;
                        } else if ( hoveredPart == HoverPart::HoldBody ) {
                            inspect.kind = HoverInspectKind::FlickBody;
                            inspect.body = makeBeatPoint(note.m_timestamp,
                                                         note.m_trackIndex);
                        } else {
                            inspect.kind  = HoverInspectKind::FlickHead;
                            inspect.head  = makeBeatPoint(note.m_timestamp,
                                                          note.m_trackIndex);
                            inspect.track = note.m_trackIndex;
                            inspect.showTrack = true;
                        }
                        inspect.showDtrack = true;
                    } else {
                        inspect.kind = HoverInspectKind::Note;
                        inspect.head =
                            makeBeatPoint(note.m_timestamp, note.m_trackIndex);
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
                double currentAbsY = cache->getAbsY(m_ctx->animateTime);
                double targetAbsY  = cache->getAbsY(snapshot->previewHoverTime);

                float renderScaleY =
                    calculatePreviewRenderScaleY(*m_ctx, camera, config);

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
        if ( isActiveSession && m_ctx->brushState.isActive ) {
            snapshot->brush.isActive     = true;
            snapshot->brush.time         = m_ctx->brushState.time;
            snapshot->brush.duration     = m_ctx->brushState.duration;
            snapshot->brush.track        = m_ctx->brushState.track;
            snapshot->brush.dtrack       = m_ctx->brushState.dtrack;
            snapshot->brush.type         = m_ctx->brushState.type;
            snapshot->brush.customColors = m_ctx->brushState.customColors;
            snapshot->brush.polylineSegments =
                m_ctx->brushState.polylineSegments;
        }

        // --- 注入橡皮擦预览状态 ---
        if ( isActiveSession && m_ctx->eraserState.isActive ) {
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
        // 使用动画时间 m_ctx->animateTime 进行剔除和位置映射
        System::NoteRenderSystem::generateSnapshot(m_ctx->noteRegistry,
                                                   m_ctx->timelineRegistry,
                                                   bpmEvents,
                                                   snapshot,
                                                   cameraId,
                                                   m_ctx->animateTime,
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
