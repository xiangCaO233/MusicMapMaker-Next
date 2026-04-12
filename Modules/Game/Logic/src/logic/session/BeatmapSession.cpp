#include "logic/BeatmapSession.h"
#include "audio/AudioManager.h"
#include "log/colorful-log.h"
#include "logic/BeatmapSyncBuffer.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/InteractionComponent.h"
#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/NoteRenderSystem.h"
#include "logic/ecs/system/NoteTransformSystem.h"
#include "logic/ecs/system/ScrollCache.h"
#include "mmm/beatmap/BeatMap.h"
#include <chrono>
#include <numeric>

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

    // 订阅音频事件
    m_audioFinishedToken =
        MMM::Event::EventBus::instance()
            .subscribe<MMM::Event::AudioFinishedEvent>(
                [this](const MMM::Event::AudioFinishedEvent& e) {
                    if ( !e.isLooping ) {
                        m_isPlaying = false;
                        // 确保 AudioManager 状态同步
                        Audio::AudioManager::instance().pause();
                    }
                });

    m_audioPositionToken =
        MMM::Event::EventBus::instance()
            .subscribe<MMM::Event::AudioPositionEvent>(
                [this](const MMM::Event::AudioPositionEvent& e) {
                    if ( m_isPlaying ) {
                        // 仅缓存位置和系统时间戳
                        m_lastAudioPos     = e.positionSeconds;
                        m_lastAudioSysTime = e.systemTimeSeconds;
                    }
                });
}

void BeatmapSession::pushCommand(LogicCommand&& cmd)
{
    m_commandQueue.enqueue(std::move(cmd));
}

void BeatmapSession::update(double dt, const Config::EditorConfig& config)
{
    m_lastConfig = config;

    // 处理来自 UI 的指令
    processCommands();

    // --- 边缘自动滚动处理 ---
    if ( std::abs(m_previewEdgeScrollVelocity) > 0.0001 ) {
        double delta     = m_previewEdgeScrollVelocity * dt;
        double totalTime = Audio::AudioManager::instance().getTotalTime();
        m_currentTime    = std::clamp(m_currentTime + delta, 0.0, totalTime);

        // 由于手动改变了时间，需要同步时钟和打击索引
        m_syncClock.reset(m_currentTime);
        if ( m_isPlaying ) {
            Audio::AudioManager::instance().seek(m_currentTime);
        }
        syncHitIndex();
    }

    double prevVisualTime = m_visualTime;

    // 更新播放时间
    if ( m_isPlaying ) {
        float speed = Audio::AudioManager::instance().getPlaybackSpeed();

        // 1. 平时：直接累加正常 ECS 周期自己的 dt，保证视觉流畅
        m_currentTime += static_cast<double>(dt * speed);
        m_syncClock.advance(dt, speed);

        // 2. 周期性：通过同步计时器修正可能的累积偏移
        m_syncTimer += dt;
        if ( m_syncTimer >= config.settings.syncConfig.syncInterval ) {
            // 使用事件回调缓存的最新硬件位置进行推演，消除事件传递延迟抖动
            double currentSysTime =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            double audioTime =
                (m_lastAudioPos > 0.0)
                    ? (m_lastAudioPos +
                       (currentSysTime - m_lastAudioSysTime) * speed)
                    : Audio::AudioManager::instance().getCurrentTime();

            // 修正视觉时间偏移 (根据配置的同步模式: Integral 或 WaterTank)
            m_syncClock.sync(audioTime, config.settings.syncConfig);

            // 将逻辑时间对齐至硬件时间，防止长期累积误差
            m_currentTime = audioTime;
            m_syncTimer   = 0.0;
        }

        // 3. 计算最终视觉时间 (包含配置的视觉偏移)
        m_visualTime = m_syncClock.getVisualTime() + config.visual.visualOffset;

        // --- 打击音效与特效触发逻辑 ---
        std::vector<System::HitFXSystem::HitEvent> triggeredEvents;

        // 检测时间跳跃（往前大跳、往回跳、或者刚刚从暂停状态恢复）
        bool isJump = (std::abs(m_visualTime - prevVisualTime) > 0.2) ||
                      (m_visualTime < prevVisualTime);

        if ( isJump ) {
            m_hitFXSystem.clearActiveEffects();
            syncHitIndex();
        } else {
            // 正常的帧推进

            // A. 音频预测触发 (提前 200ms)
            double predictWindow = 0.2;
            while ( m_nextPredictHitIndex < m_hitEvents.size() &&
                    m_hitEvents[m_nextPredictHitIndex].timestamp <=
                        (m_visualTime + predictWindow) ) {
                const auto& ev = m_hitEvents[m_nextPredictHitIndex];
                // 仅对未来且未预测过的事件进行预测
                if ( ev.timestamp > (prevVisualTime + predictWindow) ) {
                    m_hitFXSystem.triggerAudio(ev, config);
                }
                m_nextPredictHitIndex++;
            }

            // B. 视觉特效触发 (精确到帧)
            while ( m_nextHitIndex < m_hitEvents.size() &&
                    m_hitEvents[m_nextHitIndex].timestamp <= m_visualTime ) {
                const auto& ev = m_hitEvents[m_nextHitIndex];
                if ( ev.timestamp > prevVisualTime ) {
                    triggeredEvents.push_back(ev);
                }
                m_nextHitIndex++;
            }
        }
        m_hitFXSystem.update(m_visualTime, triggeredEvents, config);
    } else {
        m_visualTime = m_currentTime + config.visual.visualOffset;
        m_syncTimer  = 0.0;

        // 暂停状态下虽然不播放音效，但如果用户在拖拽进度条，我们需要实时同步
        // m_nextHitIndex，这样在重新播放的一瞬间索引就是正确的。
        if ( std::abs(m_visualTime - prevVisualTime) > 0.0001 ) {
            syncHitIndex();
        }
    }

    // 执行逻辑计算和生成渲染快照
    updateECSAndRender(config);
}

void BeatmapSession::syncHitIndex()
{
    auto it = std::lower_bound(
        m_hitEvents.begin(),
        m_hitEvents.end(),
        System::HitFXSystem::HitEvent{ m_visualTime, ::MMM::NoteType::NOTE });
    m_nextHitIndex = std::distance(m_hitEvents.begin(), it);

    // 预测索引也同步到同样的位置（预测逻辑会自动往后扫）
    m_nextPredictHitIndex = m_nextHitIndex;
}

BeatmapSession::SnapResult BeatmapSession::getSnapResult(
    double rawTime, float mouseY, const CameraInfo& camera,
    const Config::EditorConfig&                  config,
    const std::vector<const TimelineComponent*>& bpmEvents) const
{
    SnapResult result;
    int        beatDivisor = config.settings.beatDivisor;
    if ( beatDivisor <= 0 ) beatDivisor = 4;

    auto* cache = m_timelineRegistry.ctx().find<System::ScrollCache>();
    if ( !cache ) return result;

    if ( bpmEvents.empty() ) return result;

    // 只有在首个 BPM 事件之后才进行磁吸计算
    if ( rawTime < bpmEvents[0]->m_timestamp ) return result;

    float  judgmentLineY = camera.viewportHeight * config.visual.judgeline_pos;
    double currentAbsY   = cache->getAbsY(m_visualTime);

    for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
        const auto* currentBPM  = bpmEvents[i];
        double      bpmTime     = currentBPM->m_timestamp;
        double      bpmVal      = currentBPM->m_value;
        double      nextBpmTime = (i + 1 < bpmEvents.size())
                                      ? bpmEvents[i + 1]->m_timestamp
                                      : std::numeric_limits<double>::infinity();

        if ( rawTime < bpmTime && i > 0 ) continue;
        if ( rawTime > nextBpmTime ) continue;

        double beatDuration = 60.0 / (bpmVal > 0 ? bpmVal : 120.0);
        double stepDuration = beatDuration / beatDivisor;

        double relativeTime    = rawTime - bpmTime;
        double stepCount       = std::round(relativeTime / stepDuration);
        double nearestStepTime = bpmTime + stepCount * stepDuration;

        if ( nearestStepTime > nextBpmTime ) nearestStepTime = nextBpmTime;

        double snapAbsY = cache->getAbsY(nearestStepTime);
        float  snapY =
            judgmentLineY - static_cast<float>(snapAbsY - currentAbsY);

        if ( std::abs(snapY - mouseY) <= config.visual.snapThreshold ) {
            result.isSnapped   = true;
            result.snappedTime = nearestStepTime;

            // Calculate fraction
            int64_t stepInt = static_cast<int64_t>(
                std::round((nearestStepTime - bpmTime) / stepDuration));
            int beatIndex = stepInt % beatDivisor;
            if ( beatIndex < 0 ) beatIndex += beatDivisor;

            if ( beatIndex == 0 ) {
                result.numerator   = 1;
                result.denominator = 1;
            } else {
                int gcd            = std::gcd(beatIndex, beatDivisor);
                result.numerator   = beatIndex / gcd;
                result.denominator = beatDivisor / gcd;
            }

            break;
        }
    }

    return result;
}

void BeatmapSession::rebuildHitEvents()
{
    m_hitEvents.clear();
    m_nextHitIndex = 0;

    auto view     = m_noteRegistry.view<NoteComponent>();
    using HitRole = System::HitFXSystem::HitEvent::Role;

    for ( auto entity : view ) {
        const auto& note = view.get<NoteComponent>(entity);

        // 如果是子物件，它的发声逻辑在 Polyline 处理中进行（为了获取 Role）
        if ( note.m_isSubNote ) continue;

        if ( note.m_type == ::MMM::NoteType::POLYLINE ) {
            size_t subNoteCount = note.m_subNotes.size();
            for ( size_t i = 0; i < subNoteCount; ++i ) {
                const auto& sn   = note.m_subNotes[i];
                HitRole     role = HitRole::Internal;
                if ( i == 0 )
                    role = HitRole::Head;
                else if ( i == subNoteCount - 1 )
                    role = HitRole::Tail;

                int span = 1;
                if ( sn.type == ::MMM::NoteType::FLICK ) {
                    span = std::abs(sn.dtrack) + 1;
                }

                m_hitEvents.push_back({ sn.timestamp,
                                        sn.type,
                                        role,
                                        span,
                                        sn.trackIndex,
                                        sn.dtrack,
                                        sn.duration,
                                        true });
            }
        } else {
            int span = 1;
            if ( note.m_type == ::MMM::NoteType::FLICK ) {
                span = std::abs(note.m_dtrack) + 1;
            }

            m_hitEvents.push_back({ note.m_timestamp,
                                    note.m_type,
                                    HitRole::None,
                                    span,
                                    note.m_trackIndex,
                                    note.m_dtrack,
                                    note.m_duration,
                                    false });
        }
    }

    std::sort(m_hitEvents.begin(), m_hitEvents.end());
    syncHitIndex();
}

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

        if ( snapshot->isHoveringCanvas ) {
            auto* cache = m_timelineRegistry.ctx().find<System::ScrollCache>();
            if ( cache ) {
                float judgmentLineY =
                    camera.viewportHeight * config.visual.judgeline_pos;

                double currentAbsY = cache->getAbsY(m_visualTime);
                double deltaY      = (judgmentLineY - m_mouseY);

                // 核心修复：预览区的坐标是经过压缩的，计算时间时需要除以缩放比例
                if ( cameraId == "Preview" ) {
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

                    float renderScaleY =
                        previewDrawH / (mainEffectiveH *
                                        config.visual.previewConfig.areaRatio);

                    if ( std::abs(renderScaleY) > 0.0001f ) {
                        deltaY /= renderScaleY;
                    }
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
                    std::floor((m_mouseX - leftX) / singleTrackW));
                snapshot->hoveredTrack = std::clamp(track, 0, m_trackCount - 1);

                // --- 磁吸拍线时间戳预览 ---
                auto snap = getSnapResult(
                    snapshot->hoveredTime, m_mouseY, camera, config, bpmEvents);
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
                    snapshot->previewHoverY     = m_mouseY;

                    // 核心逻辑：拖动预览区时，主画布应该渲染拖拽处的内容
                    snapshot->previewHoverTime = snapshot->hoveredTime;
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
        // 使用视觉时间 m_visualTime 进行剔除和位置映射
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

}  // namespace MMM::Logic
