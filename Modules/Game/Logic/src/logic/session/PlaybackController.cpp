#include "logic/session/PlaybackController.h"
#include "audio/AudioManager.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace MMM::Logic
{
namespace
{
/// @brief 获取当前控制手势使用的 steady_clock 秒数。
[[nodiscard]] double currentSteadySeconds() noexcept
{
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// @brief 在暂停命令提交前冻结会话连续时钟，避免回读离散 block 位置。
/// @param ctx 当前会话。
/// @param audio 全局音频管理器。
/// @warning 低频播放控制路径：只读取本地锚点并提交常量级控制命令。
void pauseAndFreezeVisualClock(SessionContext& ctx, Audio::AudioManager& audio)
{
    const double now         = currentSteadySeconds();
    double       currentTime = ctx.playbackVisualClock.initialized()
                                   ? ctx.playbackVisualClock.currentTimeAt(now)
                                   : ctx.currentTime;
    const double totalTime   = SessionUtils::getEffectiveTotalTimeSeconds(ctx);
    if ( std::isfinite(totalTime) ) {
        currentTime = std::min(currentTime, totalTime);
    }
    if ( !std::isfinite(currentTime) ) {
        currentTime = 0.0;
    }
    audio.pause();
    ctx.currentTime = currentTime;
    ctx.playbackVisualClock.rebase(
        currentTime, now, audio.getPlaybackSpeed(), false);
}

/// @brief 取消未完成的鼠标编辑状态，防止进入播放后交互预览残留。
/// @param ctx 当前播放控制器所属的会话上下文。
/// @warning 低频播放切换路径：仅在开始播放前执行，只清理常量级状态容器。
void cancelActiveEditingState(SessionContext& ctx)
{
    const bool keepMarquee  = ctx.currentTool == EditTool::Marquee &&
                              ctx.isSelecting && !ctx.marqueeBoxes.empty();
    const bool keepMoveDrag = ctx.currentTool == EditTool::Move &&
                              ctx.draggedEntity != entt::null &&
                              ctx.noteRegistry.valid(ctx.draggedEntity) &&
                              ctx.dragInitialNote.has_value();
    const bool keepBrush =
        ctx.currentTool == EditTool::Draw && ctx.brushState.isActive;

    if ( keepMoveDrag ) {
        // 拖拽物件允许在播放开始后继续定位，避免播放键打断尚未提交的手势。
        ctx.isDragging = true;
    } else if ( keepBrush ) {
        // 画笔绘制允许在播放开始后继续定位，但不沿用物件拖拽的部位状态。
        ctx.isDragging  = true;
        ctx.draggedPart = HoverPart::None;
    } else {
        ctx.isDragging  = false;
        ctx.draggedPart = HoverPart::None;
        ctx.dragCameraId.clear();
    }
    if ( !keepMarquee ) {
        ctx.isSelecting             = false;
        ctx.hasMarqueeSelection     = false;
        ctx.marqueeIsAdditive       = false;
        ctx.isMarqueeSelectionDirty = false;
        ctx.marqueeBoxes.clear();
    }

    if ( !keepBrush ) {
        ctx.brushState.isActive = false;
        ctx.brushState.polylineSegments.clear();
        ctx.brushState.holdStartTime = -1.0;
        ctx.brushState.duration      = 0.0;
        ctx.brushState.dtrack        = 0;
    }

    ctx.eraserState.isActive         = false;
    ctx.eraserState.isShiftDown      = false;
    ctx.eraserState.targetObjectKind = ChartObjectKind::PlayerNote;
    ctx.eraserState.targetEntities.clear();
}
}  // namespace

void PlaybackController::handleCommand(const CmdSetPlayState& cmd)
{
    if ( !m_ctx.isActiveSession ) {
        m_ctx.isPlaying                   = false;
        m_ctx.isAudioTimelineSyncFollower = false;
        return;
    }

    const bool shouldRestartFromBeginning =
        cmd.isPlaying && m_ctx.restartPlaybackAfterFinishPending;
    if ( cmd.isPlaying ) {
        m_ctx.restartPlaybackAfterFinishPending = false;
    }

    m_ctx.isAudioTimelineSyncFollower = false;
    m_ctx.isPlaying                   = cmd.isPlaying;
    if ( m_ctx.isPlaying ) {
        if ( shouldRestartFromBeginning ) {
            m_ctx.currentTime = 0.0;
        }
        cancelActiveEditingState(m_ctx);
        if ( !SessionUtils::activateAudioTimeline(m_ctx, true) ) {
            m_ctx.isPlaying = false;
            return;
        }
        SessionUtils::syncHitIndex(m_ctx);
        m_ctx.hitFXSystem.clearActiveEffects();
    } else {
        m_ctx.restartPlaybackAfterFinishPending = false;
        auto& audio = Audio::AudioManager::instance();
        if ( audio.getLoadedAudioTimelineFingerprint() ==
             m_ctx.audioTimelineDescriptor.m_fingerprint ) {
            pauseAndFreezeVisualClock(m_ctx, audio);
        } else {
            audio.pause();
        }
    }
}

void PlaybackController::handleCommand(const CmdSeek& cmd)
{
    const bool isContinuingScrub = cmd.isScrubbing && m_isSeekScrubbing;
    m_ctx.restartPlaybackAfterFinishPending = false;
    m_ctx.isAudioTimelineSyncFollower       = false;
    if ( m_ctx.isPlaying && m_ctx.lastConfig.settings.stopPlaybackOnScroll ) {
        m_ctx.isPlaying = false;
        if ( m_ctx.isActiveSession ) {
            auto& audio = Audio::AudioManager::instance();
            if ( audio.getLoadedAudioTimelineFingerprint() ==
                 m_ctx.audioTimelineDescriptor.m_fingerprint ) {
                pauseAndFreezeVisualClock(m_ctx, audio);
            } else {
                audio.pause();
            }
        }
    }

    double totalTime = SessionUtils::getEffectiveTotalTimeSeconds(m_ctx);
    double minTime   = -m_ctx.lastConfig.visual.getEffectiveVisualOffset();

    // 核心修复：确保 std::clamp 的上限不小于下限。
    // 如果由于配置（如负的 visualOffset）导致 minTime > totalTime，
    // 我们将 minTime 限制为 totalTime，防止触发 std::clamp 的断言失败。
    if ( minTime > totalTime ) {
        minTime = totalTime;
    }

    m_ctx.currentTime = std::clamp(cmd.time, minTime, totalTime);
    if ( m_ctx.isActiveSession ) {
        auto& audio = Audio::AudioManager::instance();
        if ( m_ctx.isAudioTimelineActivationPending ||
             !audio.hasLoadedAudioTimeline() ||
             audio.getLoadedAudioTimelineFingerprint() !=
                 m_ctx.audioTimelineDescriptor.m_fingerprint ) {
            (void)SessionUtils::activateAudioTimeline(m_ctx, m_ctx.isPlaying);
        } else {
            audio.seek(m_ctx.currentTime,
                       isContinuingScrub ? Audio::AudioSeekMode::ScrubUpdate
                                         : Audio::AudioSeekMode::Commit);
            m_ctx.playbackVisualClock.rebase(m_ctx.currentTime,
                                             currentSteadySeconds(),
                                             audio.getPlaybackSpeed(),
                                             m_ctx.isPlaying);
        }
    }
    m_isSeekScrubbing = cmd.isScrubbing;
    SessionUtils::syncHitIndex(m_ctx);
    m_ctx.hitFXSystem.clearActiveEffects();
}

/// @brief 处理全局预览播放倍率切换。
/// @param cmd 设置播放倍率指令。
/// @warning 低频播放控制路径：仅活动会话可以修改全局 transport 倍率。
void PlaybackController::handleCommand(const CmdSetPlaybackSpeed& cmd)
{
    if ( !m_ctx.isActiveSession ) {
        return;
    }

    auto& audio = Audio::AudioManager::instance();
    if ( m_ctx.isPlaying ) {
        const double now = currentSteadySeconds();
        if ( m_ctx.playbackVisualClock.initialized() ) {
            m_ctx.currentTime = m_ctx.playbackVisualClock.currentTimeAt(now);
        }
        SessionUtils::syncHitIndex(m_ctx);
        audio.setPlaybackSpeed(cmd.speed);
        return;
    }

    audio.setPlaybackSpeed(cmd.speed);
}

/// @brief 应用单条玩家或 BGM 轨道的运行时 Key 音静音状态。
/// @param cmd 目标区域、轨道索引和静音状态。
/// @warning 低频 UI 控制路径；BGM 轨道修改会在音频 block 边界替换调度表。
void PlaybackController::handleCommand(const CmdSetKeySoundTrackMute& cmd)
{
    if ( !m_ctx.isActiveSession ) return;

    auto& audio = Audio::AudioManager::instance();
    if ( cmd.area == KeySoundTrackArea::Bgm ) {
        audio.setBgmKeySoundTrackMuted(cmd.trackIndex, cmd.muted);
        return;
    }
    audio.setPlayerKeySoundTrackMuted(cmd.trackIndex, cmd.muted);
}

/// @brief 应用整个 BGM 轨道区的运行时 Key 音静音状态。
/// @param cmd BGM 区静音状态。
/// @warning 低频 UI 控制路径；会在音频 block 边界替换调度表。
void PlaybackController::handleCommand(const CmdSetBgmKeySoundAreaMute& cmd)
{
    if ( !m_ctx.isActiveSession ) return;
    Audio::AudioManager::instance().setBgmKeySoundAreaMuted(cmd.muted);
}

/// @brief 处理普通时间滚动或仅应用滚动暂停策略的滚轮命令。
/// @param cmd 滚轮滚动指令。
/// @warning
/// 逻辑输入路径：用户滚轮触发时调用；同主音轨后台跟随画布在暂停开关开启时
/// 需要一并停止当前主音频，避免活动画布下一轮同步把目标时间复原。
void PlaybackController::handleCommand(const CmdScroll& cmd)
{
    float wheel = cmd.wheel;
    if ( m_ctx.lastConfig.settings.reverseScroll &&
         (SessionUtils::isMainCanvasCameraId(cmd.cameraId) ||
          cmd.cameraId == "Timeline") ) {
        wheel = -wheel;
    }

    const bool shouldStopPlayback =
        m_ctx.lastConfig.settings.stopPlaybackOnScroll &&
        (m_ctx.isPlaying || m_ctx.isAudioTimelineSyncFollower);
    if ( shouldStopPlayback ) {
        m_ctx.isPlaying                   = false;
        m_ctx.isAudioTimelineSyncFollower = false;
        if ( m_ctx.isActiveSession ) {
            auto& audio = Audio::AudioManager::instance();
            if ( audio.getLoadedAudioTimelineFingerprint() ==
                 m_ctx.audioTimelineDescriptor.m_fingerprint ) {
                pauseAndFreezeVisualClock(m_ctx, audio);
            } else {
                audio.pause();
            }
        }
        // 如果停止了播放，需要同步一下渲染状态 (虽然 seek
        // 也会做，但这里明确一下更好)
    }

    if ( cmd.intent == ScrollCommandIntent::ModifierAdjustment ) {
        return;
    }

    m_ctx.restartPlaybackAfterFinishPending = false;

    bool isShiftAccelerated = cmd.isShiftDown;
    if ( isShiftAccelerated && m_ctx.brushState.isActive &&
         m_ctx.lastConfig.settings.disableScrollAccelerationWhileDrawing ) {
        isShiftAccelerated = false;
    }

    double targetTime   = m_ctx.currentTime;
    double visualOffset = m_ctx.lastConfig.visual.getEffectiveVisualOffset();

    if ( m_ctx.lastConfig.settings.scrollSnap ) {
        int beatDivisor = m_ctx.lastConfig.settings.beatDivisor;
        if ( beatDivisor <= 0 ) beatDivisor = 4;

        SessionUtils::ensureBpmEvents(m_ctx);
        const auto& bpmEvents = m_ctx.bpmEvents;
        if ( !bpmEvents.empty() ) {
            double visualCurrentTime = m_ctx.currentTime + visualOffset;
            size_t currentIdx        = 0;
            for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
                if ( visualCurrentTime >= bpmEvents[i]->m_timestamp ) {
                    currentIdx = i;
                } else {
                    break;
                }
            }

            const auto* currentBPM = bpmEvents[currentIdx];
            double      bpmVal     = currentBPM->m_value;
            double      bVal       = bpmVal;
            if ( bVal <= 0.0 ) {
                bVal = 120.0;
                if ( m_ctx.currentBeatmap &&
                     m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm >
                         0.0 ) {
                    bVal =
                        m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm;
                }
            }
            double beatDuration = 60.0 / bVal;
            double stepDuration = isShiftAccelerated
                                      ? beatDuration
                                      : (beatDuration / beatDivisor);

            double relativeVisualTime =
                visualCurrentTime - currentBPM->m_timestamp;
            double stepCount = relativeVisualTime / stepDuration;
            double jump = std::max(1.0, static_cast<double>(std::abs(wheel)));

            double targetVisualTime = visualCurrentTime;
            if ( wheel > 0 ) {
                targetVisualTime =
                    currentBPM->m_timestamp +
                    std::floor(stepCount - 0.001 - (jump - 1.0)) * stepDuration;
            } else {
                targetVisualTime =
                    currentBPM->m_timestamp +
                    std::ceil(stepCount + 0.001 + (jump - 1.0)) * stepDuration;
            }
            targetTime = targetVisualTime - visualOffset;
        } else {
            double step = 0.25;
            if ( isShiftAccelerated )
                step *= m_ctx.lastConfig.settings.scrollSpeedMultiplier;
            targetTime = m_ctx.currentTime - static_cast<double>(wheel) * step;
        }
    } else {
        double step = 0.25;
        if ( isShiftAccelerated )
            step *= m_ctx.lastConfig.settings.scrollSpeedMultiplier;
        targetTime = m_ctx.currentTime - static_cast<double>(wheel) * step;
    }

    double totalTime = SessionUtils::getEffectiveTotalTimeSeconds(m_ctx);
    double minTime   = -m_ctx.lastConfig.visual.getEffectiveVisualOffset();

    if ( minTime > totalTime ) {
        minTime = totalTime;
    }

    m_ctx.currentTime = std::clamp(targetTime, minTime, totalTime);
    if ( m_ctx.isActiveSession ) {
        auto& audio = Audio::AudioManager::instance();
        if ( m_ctx.isAudioTimelineActivationPending ||
             !audio.hasLoadedAudioTimeline() ||
             audio.getLoadedAudioTimelineFingerprint() !=
                 m_ctx.audioTimelineDescriptor.m_fingerprint ) {
            (void)SessionUtils::activateAudioTimeline(m_ctx, m_ctx.isPlaying);
        } else {
            audio.seek(m_ctx.currentTime);
            m_ctx.playbackVisualClock.rebase(m_ctx.currentTime,
                                             currentSteadySeconds(),
                                             audio.getPlaybackSpeed(),
                                             m_ctx.isPlaying);
        }
    }
    SessionUtils::syncHitIndex(m_ctx);
    m_ctx.hitFXSystem.clearActiveEffects();
}

/// @brief 处理主画布中键二维平移。
/// @param cmd 逻辑像素空间中的平移增量和输入视口尺寸。
/// @warning 逻辑输入热路径：中键拖动期间每个 update
/// 调用；只更新单个相机并通过 ScrollCache 执行对数级反向映射。
void PlaybackController::handleCommand(const CmdPanCanvas& cmd)
{
    if ( !SessionUtils::isMainCanvasCameraId(cmd.cameraId) ) {
        return;
    }

    auto cameraIt = m_ctx.cameras.find(cmd.cameraId);
    if ( cameraIt == m_ctx.cameras.end() ) {
        if ( !std::isfinite(cmd.viewportWidth) || cmd.viewportWidth <= 0.0F ||
             !std::isfinite(cmd.viewportHeight) ||
             cmd.viewportHeight <= 0.0F ) {
            return;
        }
        cameraIt = m_ctx.cameras
                       .emplace(cmd.cameraId,
                                CameraInfo{ cmd.cameraId,
                                            cmd.viewportWidth,
                                            cmd.viewportHeight })
                       .first;
    }

    auto& camera = cameraIt->second;
    if ( std::isfinite(cmd.viewportWidth) && cmd.viewportWidth > 0.0F &&
         std::abs(camera.viewportWidth - cmd.viewportWidth) > 0.01F ) {
        camera.horizontalOffsetX = resizeCanvasHorizontalOffset(
            camera.horizontalOffsetX, camera.viewportWidth, cmd.viewportWidth);
        camera.viewportWidth = cmd.viewportWidth;
    }
    if ( std::isfinite(cmd.viewportHeight) && cmd.viewportHeight > 0.0F ) {
        camera.viewportHeight = cmd.viewportHeight;
    }

    if ( std::isfinite(cmd.deltaX) ) {
        camera.horizontalOffsetX += cmd.deltaX;
        if ( !std::isfinite(camera.horizontalOffsetX) ) {
            camera.horizontalOffsetX = 0.0F;
        }
    }

    if ( !std::isfinite(cmd.deltaY) || std::abs(cmd.deltaY) <= 0.001F ) {
        return;
    }

    const double visualOffset =
        m_ctx.lastConfig.visual.getEffectiveVisualOffset();
    const double visualTime   = m_ctx.currentTime + visualOffset;
    double       renderScaleY = static_cast<double>(cmd.renderScaleY);
    if ( !std::isfinite(renderScaleY) || std::abs(renderScaleY) <= 1e-6 ) {
        renderScaleY = 1.0;
    }

    double      targetVisualTime = visualTime;
    const auto* cache =
        m_ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    if ( cache ) {
        const double currentAbsY = cache->getVisualAnchorAbsY(visualTime);
        targetVisualTime         = cache->getTime(
            currentAbsY + static_cast<double>(cmd.deltaY) / renderScaleY);
    } else {
        constexpr double FALLBACK_PIXELS_PER_SECOND = 500.0;
        targetVisualTime += static_cast<double>(cmd.deltaY) / renderScaleY /
                            FALLBACK_PIXELS_PER_SECOND;
    }

    if ( !std::isfinite(targetVisualTime) ) {
        return;
    }

    handleCommand(CmdSeek{ targetVisualTime - visualOffset });

    // 直接操作期间不叠加滚动动画，否则视觉内容会落后于中键指针。
    m_ctx.animateTime                = m_ctx.currentTime + visualOffset;
    m_ctx.animateTimeTarget          = m_ctx.animateTime;
    m_ctx.animateTimeAnimationActive = false;
}


}  // namespace MMM::Logic
