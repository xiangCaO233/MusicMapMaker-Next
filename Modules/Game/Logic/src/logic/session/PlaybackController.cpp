#include "logic/session/PlaybackController.h"
#include "audio/AudioManager.h"
#include "config/Utf8Path.h"
#include "log/colorful-log.h"
#include "logic/EditorEngine.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/project/Project.h"
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace MMM::Logic
{
namespace
{
/// @brief 判断项目音频资源是否匹配谱面主音频路径。
/// @param resource 待匹配的项目音频资源。
/// @param mainAudioPath 谱面元数据中保存的主音频路径。
/// @return ID、原始路径或通用分隔符路径任一匹配时返回 true。
bool matchesMainAudioPath(const AudioResource&         resource,
                          const std::filesystem::path& mainAudioPath)
{
    const std::string audioFileName =
        Config::pathToUtf8(mainAudioPath.filename());
    const std::string audioPath = Config::pathToUtf8(mainAudioPath);
    const std::string genericAudioPath =
        Config::pathToUtf8Generic(mainAudioPath);

    return resource.m_id == audioFileName || resource.m_path == audioPath ||
           resource.m_path == genericAudioPath;
}

/// @brief 查找当前谱面主音频在项目资源表中的轨道配置。
/// @param project 当前项目。
/// @param mainAudioPath 谱面元数据中保存的主音频路径。
/// @return 匹配到的音轨配置；找不到时返回默认配置。
AudioTrackConfig findMainAudioConfig(const Project&               project,
                                     const std::filesystem::path& mainAudioPath)
{
    for ( const auto& resource : project.m_audioResources ) {
        if ( matchesMainAudioPath(resource, mainAudioPath) ) {
            return resource.m_config;
        }
    }
    return {};
}

/// @brief 将当前播放倍率写回当前谱面主音轨的项目资源配置。
/// @param ctx 当前播放控制器所属的会话上下文。
/// @param playbackSpeed 已应用到音频管理器的播放倍率。
/// @warning
/// 低频播放控制路径：仅在用户修改播放倍率时执行，遍历项目音频资源表，不放入每帧
/// update。
void syncMainAudioPlaybackSpeedToProjectResource(SessionContext& ctx,
                                                 double          playbackSpeed)
{
    if ( !ctx.currentBeatmap ) {
        return;
    }

    auto* project = EditorEngine::instance().getCurrentProject();
    if ( !project ) {
        return;
    }

    const auto& mainAudioPath =
        ctx.currentBeatmap->m_baseMapMetadata.main_audio_path;
    if ( mainAudioPath.empty() ) {
        return;
    }

    for ( auto& resource : project->m_audioResources ) {
        if ( !matchesMainAudioPath(resource, mainAudioPath) ) {
            continue;
        }

        resource.m_config.playbackSpeed = static_cast<float>(playbackSpeed);
        return;
    }
}

/// @brief 确保播放前 AudioManager 已加载当前谱面的主音频。
/// @param ctx 当前播放控制器所属的会话上下文。
/// @return 已加载或成功补加载时返回 true；没有可用主音频时返回 false。
/// @warning
/// 低频播放控制路径：仅在用户切换到播放态时执行，可能访问文件系统并触发音频解码缓存加载，禁止放入每帧
/// update。
bool ensureCurrentBeatmapBgmLoaded(SessionContext& ctx)
{
    if ( !ctx.currentBeatmap ) {
        ctx.loadedMainAudioPath.clear();
        ctx.mainAudioTotalTime = 0.0;
        return false;
    }

    const auto& meta = ctx.currentBeatmap->m_baseMapMetadata;
    if ( meta.main_audio_path.empty() ) {
        ctx.loadedMainAudioPath.clear();
        ctx.mainAudioTotalTime = 0.0;
        return false;
    }

    const auto* project   = EditorEngine::instance().getCurrentProject();
    auto        audioPath = SessionUtils::resolveMainAudioPath(ctx, project);
    std::error_code filesystemError;
    const bool      isAudioFile =
        std::filesystem::is_regular_file(audioPath, filesystemError);
    if ( filesystemError || !isAudioFile ) {
        ctx.loadedMainAudioPath.clear();
        ctx.mainAudioTotalTime = 0.0;
        XWARN("PlaybackController: main audio file is unavailable: {}",
              Config::pathToUtf8(audioPath));
        return false;
    }

    auto&             audio         = Audio::AudioManager::instance();
    const std::string audioPathUtf8 = Config::pathToUtf8(audioPath);
    if ( audio.getLoadedBGMPath() != audioPathUtf8 ) {
        AudioTrackConfig config;
        if ( project ) {
            config = findMainAudioConfig(*project, meta.main_audio_path);
        }
        if ( !audio.loadBGM(audioPathUtf8, config) ) {
            ctx.loadedMainAudioPath.clear();
            ctx.mainAudioTotalTime = 0.0;
            XERROR("PlaybackController: failed to load main audio: {}",
                   audioPathUtf8);
            return false;
        }
    }

    ctx.loadedMainAudioPath = audioPathUtf8;
    ctx.mainAudioTotalTime  = audio.getTotalTime();
    audio.seek(ctx.currentTime);
    return true;
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

    ctx.eraserState.isActive    = false;
    ctx.eraserState.isShiftDown = false;
    ctx.eraserState.targetEntities.clear();
}
}  // namespace

void PlaybackController::handleCommand(const CmdSetPlayState& cmd)
{
    m_ctx.isMainAudioSyncFollower = false;
    m_ctx.isPlaying               = cmd.isPlaying;
    if ( m_ctx.isPlaying ) {
        cancelActiveEditingState(m_ctx);
        m_ctx.syncTimer             = 0.0;
        m_ctx.lastAudioPos          = 0.0;
        m_ctx.lastAudioSysTime      = 0.0;
        m_ctx.hasInitialAudioOffset = false;
        // 初始化壁钟基准，用于后续无抖动的 visualTime 计算
        m_ctx.playStartSysTime =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        m_ctx.playStartVisualTime = m_ctx.currentTime;
        (void)ensureCurrentBeatmapBgmLoaded(m_ctx);
        Audio::AudioManager::instance().play();
        m_ctx.syncClock.reset(m_ctx.currentTime);
        SessionUtils::syncHitIndex(m_ctx);
        m_ctx.hitFXSystem.clearActiveEffects();
    } else {
        Audio::AudioManager::instance().pause();
        m_ctx.currentTime = Audio::AudioManager::instance().getCurrentTime();
    }
}

void PlaybackController::handleCommand(const CmdSeek& cmd)
{
    m_ctx.isMainAudioSyncFollower = false;
    if ( m_ctx.isPlaying && m_ctx.lastConfig.settings.stopPlaybackOnScroll ) {
        m_ctx.isPlaying = false;
        Audio::AudioManager::instance().pause();
        m_ctx.currentTime = Audio::AudioManager::instance().getCurrentTime();
    }

    double totalTime = SessionUtils::getEffectiveTotalTimeSeconds(m_ctx);
    double minTime   = -m_ctx.lastConfig.visual.getEffectiveVisualOffset();

    // 核心修复：确保 std::clamp 的上限不小于下限。
    // 如果由于配置（如负的 visualOffset）导致 minTime > totalTime，
    // 我们将 minTime 限制为 totalTime，防止触发 std::clamp 的断言失败。
    if ( minTime > totalTime ) {
        minTime = totalTime;
    }

    m_ctx.currentTime           = std::clamp(cmd.time, minTime, totalTime);
    m_ctx.lastAudioPos          = 0.0;
    m_ctx.lastAudioSysTime      = 0.0;
    m_ctx.hasInitialAudioOffset = false;
    // 重置壁钟基准
    m_ctx.playStartSysTime =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    m_ctx.playStartVisualTime = m_ctx.currentTime;
    m_ctx.syncClock.reset(m_ctx.currentTime);
    Audio::AudioManager::instance().seek(m_ctx.currentTime);
    SessionUtils::syncHitIndex(m_ctx);
    m_ctx.hitFXSystem.clearActiveEffects();
}

/// @brief 处理播放倍率切换，并同步当前主音轨的项目资源配置。
/// @param cmd 设置播放倍率指令。
/// @warning
/// 低频播放控制路径：仅在用户修改播放倍率时执行；播放中会重置同步基准，不放入每帧
/// update。
void PlaybackController::handleCommand(const CmdSetPlaybackSpeed& cmd)
{
    auto& audio    = Audio::AudioManager::instance();
    float oldSpeed = static_cast<float>(audio.getPlaybackSpeed());
    if ( std::abs(static_cast<float>(cmd.speed) - oldSpeed) < 1e-6f ) {
        syncMainAudioPlaybackSpeedToProjectResource(m_ctx, oldSpeed);
        return;
    }

    if ( m_ctx.isPlaying ) {
        // 获取当前系统时间
        double currentSysTime =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();

        // 1. 在切换速度前，以旧速度计算出当前的精确逻辑时间
        m_ctx.currentTime =
            m_ctx.playStartVisualTime +
            (currentSysTime - m_ctx.playStartSysTime) * oldSpeed;

        // 2. 以当前逻辑时间作为新速度的起点，重置系统时钟基准
        m_ctx.playStartVisualTime = m_ctx.currentTime;
        m_ctx.playStartSysTime    = currentSysTime;

        // 3. 强制重置音频同步系统，使其在变速后立即重新对齐硬件时钟
        m_ctx.hasInitialAudioOffset = false;
        // 将计时器设为间隔值，确保在下一次 BeatmapSession::update
        // 中立即触发同步块
        m_ctx.syncTimer = m_ctx.lastConfig.settings.syncConfig.syncInterval;

        m_ctx.syncClock.reset(m_ctx.currentTime);
        SessionUtils::syncHitIndex(m_ctx);
    }

    audio.setPlaybackSpeed(cmd.speed);
    syncMainAudioPlaybackSpeedToProjectResource(m_ctx,
                                                audio.getPlaybackSpeed());
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
        (m_ctx.isPlaying || m_ctx.isMainAudioSyncFollower);
    if ( shouldStopPlayback ) {
        m_ctx.isPlaying               = false;
        m_ctx.isMainAudioSyncFollower = false;
        Audio::AudioManager::instance().pause();
        m_ctx.currentTime = Audio::AudioManager::instance().getCurrentTime();
        // 如果停止了播放，需要同步一下渲染状态 (虽然 seek
        // 也会做，但这里明确一下更好)
    }

    if ( cmd.intent == ScrollCommandIntent::ModifierAdjustment ) {
        return;
    }

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

    m_ctx.currentTime           = std::clamp(targetTime, minTime, totalTime);
    m_ctx.lastAudioPos          = 0.0;
    m_ctx.lastAudioSysTime      = 0.0;
    m_ctx.hasInitialAudioOffset = false;
    // 重置壁钟基准
    m_ctx.playStartSysTime =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    m_ctx.playStartVisualTime = m_ctx.currentTime;
    m_ctx.syncClock.reset(m_ctx.currentTime);
    Audio::AudioManager::instance().seek(m_ctx.currentTime);
    SessionUtils::syncHitIndex(m_ctx);
    m_ctx.hitFXSystem.clearActiveEffects();
}


}  // namespace MMM::Logic
