#include "logic/session/PlaybackController.h"
#include "audio/AudioManager.h"
#include "common/LogicCommands.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/CanvasCamera.h"
#include "logic/session/SessionUtils.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/beatmap/BeatMap.h"
#include "mmm/timing/BpmNormalization.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace MMM::Logic
{
namespace
{
/// @brief 获取当前控制手势使用的 steady_clock 秒数。
/// @return 与视觉时钟锚点相同时间基准的秒数，不是谱面时间。
[[nodiscard]] double currentSteadySeconds() noexcept
{
    // 控制事件的时间戳使用单调时钟，系统校时不应造成播放进度跳变。
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// @brief 在暂停命令提交前冻结会话连续时钟，避免回读离散 block 位置。
/// @param ctx 当前会话。
/// @param audio 全局音频管理器。
/// @pre 调用者已确认加载的音频描述符属于该会话。
/// @details 冻结的是视觉连续位置，不使用音频块边界覆盖暂停位置。
/// @warning 低频播放控制路径：只读取本地锚点并提交常量级控制命令。
void pauseAndFreezeVisualClock(SessionContext& ctx, Audio::AudioManager& audio)
{
    // 先按旧播放状态求暂停瞬间的位置，再向音频线程发布暂停。
    // 初次激活前尚无连续时钟时，保留会话已经选定的时间。
    const double now         = currentSteadySeconds();
    double       currentTime = ctx.playbackVisualClock.initialized()
                                   ? ctx.playbackVisualClock.currentTimeAt(now)
                                   : ctx.currentTime;
    const double totalTime   = SessionUtils::getEffectiveTotalTimeSeconds(ctx);
    if ( std::isfinite(totalTime) ) {
        // 连续外推可能越过曲尾；暂停不能把越界位置留下作为下次起点。
        // 此处不钳制到零，视觉偏移允许会话停留在负的前导时间。
        currentTime = std::min(currentTime, totalTime);
    }
    if ( !std::isfinite(currentTime) ) {
        // 无效锚点不能传播到停止态时钟，回到可用的谱面原点。
        currentTime = 0.0;
    }
    audio.pause();
    ctx.currentTime = currentTime;
    // 冻结锚点与上面的取样共用 now，避免两次取时产生额外位移。
    ctx.playbackVisualClock.rebase(
        currentTime, now, audio.getPlaybackSpeed(), false);
}

/// @brief 取消未完成的鼠标编辑状态，防止进入播放后交互预览残留。
/// @param ctx 当前播放控制器所属的会话上下文。
/// @details
/// 保留仍有效的移动、框选与绘制手势，取消其他临时状态；不提交编辑命令。
/// @warning 仅在开始播放前执行；清理手势容器，不应移入逐帧更新。
void cancelActiveEditingState(SessionContext& ctx)
{
    // 允许继续的手势必须与当前工具一致，并仍有有效的工作数据。
    // 单独残留的 isDragging 或 isSelecting 标志不足以恢复一次编辑。
    const bool keepMarquee = ctx.currentTool == EditTool::Marquee &&
                             ctx.isSelecting && !ctx.marqueeBoxes.empty();
    const bool keepMoveDrag = ctx.currentTool == EditTool::Move &&
                              ctx.draggedEntity != entt::null &&
                              ctx.noteRegistry.valid(ctx.draggedEntity) &&
                              ctx.dragInitialNote.has_value();
    // 笔刷的中间数据由当前绘制手势拥有，不要求已有实体才能延续。
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
        // 无可延续的手势时同时解除相机归属，避免后续输入沿用旧视口。
        ctx.isDragging  = false;
        ctx.draggedPart = HoverPart::None;
        ctx.dragCameraId.clear();
    }
    if ( !keepMarquee ) {
        // 框选模式、增量标记和矩形集合是一组状态，需要一起失效。
        // 此处清除的是框选预览，不改变谱面中已经选中的实体集合。
        ctx.isSelecting             = false;
        ctx.hasMarqueeSelection     = false;
        ctx.marqueeIsAdditive       = false;
        ctx.isMarqueeSelectionDirty = false;
        ctx.marqueeBoxes.clear();
    }

    if ( !keepBrush ) {
        // 清掉长条起点与折线段，下一次绘制不继承上次尚未提交的形状。
        ctx.brushState.isActive = false;
        ctx.brushState.polylineSegments.clear();
        ctx.brushState.holdStartTime = -1.0;
        ctx.brushState.duration      = 0.0;
        ctx.brushState.dtrack        = 0;
    }

    // 擦除不跨播放切换延续，防止旧的候选实体集合在新时间点被提交。
    ctx.eraserState.isActive         = false;
    ctx.eraserState.isShiftDown      = false;
    ctx.eraserState.targetObjectKind = ChartObjectKind::PlayerNote;
    ctx.eraserState.targetEntities.clear();
}

/// @brief 在播放控制命令清空特效后补建当前位置仍有效的 Hold 特效。
/// @param ctx 当前播放控制器所属的会话上下文。
/// @pre 调用者已将会话时间设置到跳转目标，并清空旧的活动特效。
/// @details 补建只处理当前位置的持续效果，不补播跳过区间中的瞬时命中。
/// @warning 低频播放控制路径：仅在 Start、Seek 或滚动跳转后线性扫描
/// hitEvents，禁止在普通 update 热路径调用。
void restoreActiveHoldEffectsAfterPlaybackJump(SessionContext& ctx)
{
    // 暂停定位只展示静态谱面，不恢复持续播放中的长条打击效果。
    if ( !ctx.isPlaying ) return;
    // 跳转可能跨过长条头；仅重置命中游标不足以重新建立持续特效。
    SessionUtils::ensureHitEvents(ctx);
    const double animateTime =
        ctx.currentTime + ctx.lastConfig.visual.getEffectiveVisualOffset();
    // 特效按画面判定线的位置恢复，而不是直接使用未加偏移的音频时间。
    ctx.hitFXSystem.restoreActiveHoldEffects(
        animateTime, ctx.hitEvents, ctx.lastConfig);
}
}  // namespace

/// @brief 切换当前会话的播放状态，并恢复相应的命中与视觉状态。
/// @param cmd 请求的播放状态；曲终后的首次开始请求会回到原点。
/// @details 播放依赖音频时间线激活成功，暂停不卸载已加载的资源。
/// @warning
/// 用户播放控制路径，可能激活音频时间线和重建命中特效；禁止逐帧重复提交。
void PlaybackController::handleCommand(const CmdSetPlayState& cmd)
{
    if ( !m_ctx.isActiveSession ) {
        // 后台会话不能抢占全局音频；这里只撤销本会话的播放和跟随标记。
        m_ctx.isPlaying                   = false;
        m_ctx.isAudioTimelineSyncFollower = false;
        m_ctx.m_audioTimelineSyncSourceFingerprint.clear();
        return;
    }

    // 曲终重播不同于普通暂停续播，只在待重启标记存在时重置时间。
    const bool shouldRestartFromBeginning =
        cmd.isPlaying && m_ctx.restartPlaybackAfterFinishPending;
    if ( cmd.isPlaying ) {
        // 消费一次性重播标记，即使后续激活失败也不反复强制回到曲首。
        m_ctx.restartPlaybackAfterFinishPending = false;
    }

    // 显式播放操作接管本地位置，不再从其他同音轨会话复制进度。
    m_ctx.isAudioTimelineSyncFollower = false;
    m_ctx.m_audioTimelineSyncSourceFingerprint.clear();
    m_ctx.isPlaying = cmd.isPlaying;
    if ( m_ctx.isPlaying ) {
        if ( shouldRestartFromBeginning ) {
            m_ctx.currentTime = 0.0;
        }
        cancelActiveEditingState(m_ctx);
        if ( !SessionUtils::activateAudioTimeline(m_ctx, true) ) {
            // 激活失败不能留下正在播放的 UI 状态；当前位置仍可用于重试。
            m_ctx.isPlaying = false;
            return;
        }
        // 清除上次播放的瞬时效果，再补回跨越当前位置的持续长条效果。
        // 命中游标同步到新起点，避免把此前的事件重新作为新命中触发。
        SessionUtils::syncHitIndex(m_ctx);
        m_ctx.hitFXSystem.clearActiveEffects();
        restoreActiveHoldEffectsAfterPlaybackJump(m_ctx);
    } else {
        m_ctx.restartPlaybackAfterFinishPending = false;
        // 只在资源身份一致时冻结本地视觉时钟，避免跨谱面混用播放位置。
        auto& audio = Audio::AudioManager::instance();
        if ( audio.getLoadedAudioTimelineFingerprint() ==
             m_ctx.audioTimelineDescriptor.m_fingerprint ) {
            pauseAndFreezeVisualClock(m_ctx, audio);
        } else {
            // 音频属于另一描述符时只暂停它，不用它推算当前会话的时间。
            audio.pause();
        }
    }
}

/// @brief 定位会话时间，必要时同步全局音频并重建跳转处的特效状态。
/// @param cmd 谱面秒数与是否仍在连续拖动定位的标记。
/// @details 后台定位只更新会话；活动会话同时负责切换或定位音频时间线。
/// @note 时间下界由视觉偏移决定，不固定为零；上界由有效资源时长决定。
/// @pre cmd.time 应为有限秒数；范围钳制不承担 NaN 输入清洗。
/// @warning 输入命令路径；拖动会反复调用，不能增加阻塞等待或延迟本地定位反馈。
void PlaybackController::handleCommand(const CmdSeek& cmd)
{
    // 首个拖动请求和最终释放都采用提交模式，只有连续拖动中间帧可合并。
    const bool isContinuingScrub = cmd.isScrubbing && m_ctx.isSeekScrubbing;
    // 命令入口统一解除同步跟随，拖动中间帧同样代表用户主动选择位置。
    // 显式选定位置后应从该处继续，而不是沿用曲终重播或后台跟随状态。
    m_ctx.restartPlaybackAfterFinishPending = false;
    m_ctx.isAudioTimelineSyncFollower       = false;
    m_ctx.m_audioTimelineSyncSourceFingerprint.clear();
    if ( m_ctx.isPlaying && m_ctx.lastConfig.settings.stopPlaybackOnScroll ) {
        // 跳转复用滚动暂停偏好；非活动会话只改变本地状态，不暂停全局音频。
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

    // 下界使视觉时间最早落在零；负偏移可能使该下界超过有效曲尾。
    // 将空区间收缩到曲尾，维持 std::clamp 要求的上下界次序。
    if ( minTime > totalTime ) {
        minTime = totalTime;
    }

    // 先发布本地位置；后台会话也能定位，但只有活动会话驱动音频。
    m_ctx.currentTime = std::clamp(cmd.time, minTime, totalTime);
    if ( m_ctx.isActiveSession ) {
        auto& audio = Audio::AudioManager::instance();
        if ( m_ctx.isAudioTimelineActivationPending ||
             !audio.hasLoadedAudioTimeline() ||
             audio.getLoadedAudioTimelineFingerprint() !=
                 m_ctx.audioTimelineDescriptor.m_fingerprint ) {
            // 切换中的或尚未加载的时间线必须先激活，不能向旧资源直接 seek。
            (void)SessionUtils::activateAudioTimeline(m_ctx, m_ctx.isPlaying);
        } else {
            // 已加载相同资源时保留时间线，仅发布定位请求。
            // 未在拖动中或刚进入拖动的命令都必须建立明确的提交起点。
            audio.seek(m_ctx.currentTime,
                       isContinuingScrub ? Audio::AudioSeekMode::ScrubUpdate
                                         : Audio::AudioSeekMode::Commit);
            // 立即更新画面锚点，不等待音频回调确认才让拖动产生视觉反馈。
            m_ctx.playbackVisualClock.rebase(m_ctx.currentTime,
                                             currentSteadySeconds(),
                                             audio.getPlaybackSpeed(),
                                             m_ctx.isPlaying);
        }
    }
    // 记录本次手势阶段供下一条命令判定，结束帧会解除连续更新模式。
    m_ctx.isSeekScrubbing = cmd.isScrubbing;
    // 跳转不沿用旧的瞬时特效；正在播放的跨点长条需要单独恢复。
    SessionUtils::syncHitIndex(m_ctx);
    m_ctx.hitFXSystem.clearActiveEffects();
    restoreActiveHoldEffectsAfterPlaybackJump(m_ctx);
}

/// @brief 处理全局预览播放倍率切换。
/// @param cmd 设置播放倍率指令。
/// @details 不缩放谱面物件的时间戳，倍率仅作用于预览传输。
/// @note 播放中先同步旧倍率的位置；倍率有效性由音频控制入口处理。
/// @warning 低频播放控制路径：仅活动会话可以修改全局 transport 倍率。
void PlaybackController::handleCommand(const CmdSetPlaybackSpeed& cmd)
{
    // 音频倍率是全局预览状态，后台画布不能以自己的配置覆盖它。
    if ( !m_ctx.isActiveSession ) {
        return;
    }

    auto& audio = Audio::AudioManager::instance();
    if ( m_ctx.isPlaying ) {
        // 先用旧倍率取得切换瞬间的位置，避免后续同步从过时进度开始。
        const double now = currentSteadySeconds();
        if ( m_ctx.playbackVisualClock.initialized() ) {
            m_ctx.currentTime = m_ctx.playbackVisualClock.currentTimeAt(now);
        }
        // 尚无时钟锚点时沿用会话位置，不从未初始化的时钟读取零值。
        // 倍率切换不是跳回上一音频块，命中游标也应跟随连续时间前进。
        SessionUtils::syncHitIndex(m_ctx);
        audio.setPlaybackSpeed(cmd.speed);
        return;
    }

    // 停止态只更新下次播放的倍率，不改变用户选定的位置。
    audio.setPlaybackSpeed(cmd.speed);
}

/// @brief 应用单条玩家、草稿或 BGM 轨道的运行时 Key 音静音状态。
/// @param cmd 目标区域、轨道索引和静音状态。
/// @details 轨道号属于指定区域，控制器不进行跨区域的索引偏移换算。
/// @note 静音仅影响试听，保留物件、资源绑定及原始音量配置。
/// @warning 低频 UI 控制路径；只发布固定大小原子控制状态。
void PlaybackController::handleCommand(const CmdSetKeySoundTrackMute& cmd)
{
    // 全局混音状态由活动会话管理，忽略排队期间已经失去活动权的请求。
    if ( !m_ctx.isActiveSession ) return;

    auto& audio = Audio::AudioManager::instance();
    if ( cmd.area == KeySoundTrackArea::Bgm ) {
        // 区域内的轨道索引独立编号，不能当作玩家轨道索引直接下发。
        audio.setBgmKeySoundTrackMuted(cmd.trackIndex, cmd.muted);
        return;
    }
    if ( cmd.area == KeySoundTrackArea::Draft ) {
        // 草稿轨道有独立控制区，不借用谱面玩家轨道的静音位。
        audio.setDraftKeySoundTrackMuted(cmd.trackIndex, cmd.muted);
        return;
    }
    audio.setPlayerKeySoundTrackMuted(cmd.trackIndex, cmd.muted);
}

/// @brief 应用单条玩家、草稿或 BGM 轨道的运行时 Key 音增益。
/// @param cmd 目标区域、轨道索引和线性增益。
/// @details 不在此处执行分贝换算，命令已使用音频控制接口的线性单位。
/// @note 区域整体静音与逐轨增益是独立控制项，修改增益不隐式解除静音。
/// @warning 低频 UI 控制路径；只发布固定大小原子控制状态。
void PlaybackController::handleCommand(const CmdSetKeySoundTrackGain& cmd)
{
    // 这里只调整试听混音，不改写谱面物件自身的音量属性。
    if ( !m_ctx.isActiveSession ) return;

    auto& audio = Audio::AudioManager::instance();
    if ( cmd.area == KeySoundTrackArea::Bgm ) {
        // 增益与静音使用相同的区域路由，保证面板控制作用于同一条轨道。
        audio.setBgmKeySoundTrackGain(cmd.trackIndex, cmd.gain);
        return;
    }
    if ( cmd.area == KeySoundTrackArea::Draft ) {
        audio.setDraftKeySoundTrackGain(cmd.trackIndex, cmd.gain);
        return;
    }
    audio.setPlayerKeySoundTrackGain(cmd.trackIndex, cmd.gain);
}

/// @brief 应用绑定或未绑定打击音效类别的实时增益。
/// @param cmd 目标类别和线性增益。
/// @details 此处按音效绑定类别控制，不按玩家、草稿或 BGM 轨道编号选择。
/// @warning 低频 UI 控制路径；只发布一个固定大小原子控制字。
void PlaybackController::handleCommand(const CmdSetKeySoundEffectGroupGain& cmd)
{
    if ( !m_ctx.isActiveSession ) return;

    // 逻辑命令枚举在边界处转换为音频枚举，不依赖两者底层数值相同。
    const auto group = cmd.group == KeySoundEffectGroup::Bound
                           ? Audio::KeySoundEffectGroup::Bound
                           : Audio::KeySoundEffectGroup::Unbound;
    Audio::AudioManager::instance().setKeySoundEffectGroupGain(group, cmd.gain);
}

/// @brief 应用整个草稿轨道区的运行时 Key 音静音状态。
/// @param cmd 草稿区静音状态。
/// @details 作用于草稿区整体输出，不删除草稿样本或更改项目草稿数据。
/// @warning 低频 UI 控制路径；只发布一个固定大小原子控制字。
void PlaybackController::handleCommand(const CmdSetDraftKeySoundAreaMute& cmd)
{
    // 区域总开关独立于各轨道的静音值，不遍历并覆盖逐轨设置。
    if ( !m_ctx.isActiveSession ) return;
    Audio::AudioManager::instance().setDraftKeySoundAreaMuted(cmd.muted);
}

/// @brief 应用整个 BGM 轨道区的运行时 Key 音静音状态。
/// @param cmd BGM 区静音状态。
/// @details 此开关控制 BGM 区的 Key 音，不通过停止全局传输实现静音。
/// @warning 低频 UI 控制路径；只发布一个固定大小原子控制字。
void PlaybackController::handleCommand(const CmdSetBgmKeySoundAreaMute& cmd)
{
    // 与单轨静音分开发布，恢复区域播放时仍保留原有逐轨选择。
    if ( !m_ctx.isActiveSession ) return;
    Audio::AudioManager::instance().setBgmKeySoundAreaMuted(cmd.muted);
}

/// @brief 处理普通时间滚动或仅应用滚动暂停策略的滚轮命令。
/// @param cmd 滚轮滚动指令。
/// @details 正向滚轮向较早时间移动，反向偏好会先翻转输入，再执行吸附。
/// @note 修饰键调整意图只执行暂停策略，不更新定位和命中特效。
/// @pre 普通时间滚动应提供有限的非零滚轮增量。
/// @warning
/// 逻辑输入路径：用户滚轮触发时调用；同主音轨后台跟随画布在暂停开关开启时
/// 需要一并停止当前主音频，避免活动画布下一轮同步把目标时间复原。
void PlaybackController::handleCommand(const CmdScroll& cmd)
{
    // 反向滚动偏好只作用于主画布和时间线，不改变其他视口的输入约定。
    float wheel = cmd.wheel;
    if ( m_ctx.lastConfig.settings.reverseScroll &&
         (SessionUtils::isMainCanvasCameraId(cmd.cameraId) ||
          cmd.cameraId == "Timeline") ) {
        wheel = -wheel;
    }

    // 后台同音轨跟随也视作播放：仅看 isPlaying 会漏掉跟随中的画布。
    const bool shouldStopPlayback =
        m_ctx.lastConfig.settings.stopPlaybackOnScroll &&
        (m_ctx.isPlaying || m_ctx.isAudioTimelineSyncFollower);
    if ( shouldStopPlayback ) {
        // 跟随标记与来源指纹同时清空，否则后续同步仍可能认领这个会话。
        m_ctx.isPlaying                   = false;
        m_ctx.isAudioTimelineSyncFollower = false;
        m_ctx.m_audioTimelineSyncSourceFingerprint.clear();
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

    if ( cmd.intent == ScrollCommandIntent::ModifierAdjustment ) {
        // 修饰键滚轮可触发暂停策略，但它本身不是沿时间轴移动的请求。
        // 返回放在暂停之后，避免调整参数时仍被下一帧音频跟随覆盖。
        return;
    }

    m_ctx.restartPlaybackAfterFinishPending = false;

    // 实际时间滚动取代曲终后的重启位置；仅修饰键调整不会消费这个标记。
    bool isShiftAccelerated = cmd.isShiftDown;
    if ( isShiftAccelerated && m_ctx.brushState.isActive &&
         m_ctx.lastConfig.settings.disableScrollAccelerationWhileDrawing ) {
        // 绘制期间 Shift 可能承担形状约束，按偏好避免同时放大滚动步长。
        isShiftAccelerated = false;
    }

    double targetTime   = m_ctx.currentTime;
    double visualOffset = m_ctx.lastConfig.visual.getEffectiveVisualOffset();

    if ( m_ctx.lastConfig.settings.scrollSnap ) {
        // 非正分拍数不能参与除法，使用四分拍作为可用的吸附密度。
        int beatDivisor = m_ctx.lastConfig.settings.beatDivisor;
        if ( beatDivisor <= 0 ) beatDivisor = 4;

        // 只按事件缓存的维护规则确保可用，不在这里另行建立一套 BPM 数据。
        SessionUtils::ensureBpmEvents(m_ctx);
        const auto& bpmEvents = m_ctx.bpmEvents;
        if ( !bpmEvents.empty() ) {
            // 吸附依据画面上可见的节拍线，计算前先从音频时间转到视觉时间。
            double visualCurrentTime = m_ctx.currentTime + visualOffset;
            size_t currentIdx        = 0;
            // 事件按时间排列；选最后一个不晚于当前位置的 BPM 作为网格原点。
            // 在首事件之前仍沿首段网格外推，以支持谱面前导区定位。
            for ( size_t i = 0; i < bpmEvents.size(); ++i ) {
                if ( visualCurrentTime >= bpmEvents[i]->m_timestamp ) {
                    currentIdx = i;
                } else {
                    break;
                }
            }

            const auto*  currentBPM = bpmEvents[currentIdx];
            const double fallbackBpm =
                m_ctx.currentBeatmap
                    ? m_ctx.currentBeatmap->m_baseMapMetadata.preference_bpm
                    : ::MMM::DEFAULT_NORMALIZED_BPM;
            // 事件值通过统一 BPM 规范化入口，避免异常 BPM 产生无效拍长。
            // 有谱面时提供其偏好值作为上下文，无谱面才提供默认值。
            const double bVal =
                ::MMM::normalizeBpmValue(currentBPM->m_value, fallbackBpm);
            // 加速吸附按整拍移动，普通吸附按当前分拍数划分一拍。
            double beatDuration = 60.0 / bVal;
            double stepDuration = isShiftAccelerated
                                      ? beatDuration
                                      : (beatDuration / beatDivisor);

            double relativeVisualTime =
                visualCurrentTime - currentBPM->m_timestamp;
            // 局部格数可以为负，向前用 floor、向后用 ceil 才能保持方向一致。
            double stepCount = relativeVisualTime / stepDuration;
            // 高分辨率滚轮的非零小量也至少跨一格；更大的增量保留多格跳转。
            double jump = std::max(1.0, static_cast<double>(std::abs(wheel)));

            double targetVisualTime = visualCurrentTime;
            // 微小偏置让恰好落在线上的位置继续跨到相邻线，而非再次吸到原线。
            // 先按方向取整，再乘步长，网格始终以当前 BPM 事件为起点。
            // 偏置的单位是格数，因此会随 BPM 与分拍密度一起缩放。
            if ( wheel > 0 ) {
                targetVisualTime =
                    currentBPM->m_timestamp +
                    std::floor(stepCount - 0.001 - (jump - 1.0)) * stepDuration;
            } else {
                targetVisualTime =
                    currentBPM->m_timestamp +
                    std::ceil(stepCount + 0.001 + (jump - 1.0)) * stepDuration;
            }
            // 本次使用当前段拍长，下一次输入再按新位置选择节拍段。
            // 写回前撤销显示偏移，不能把视觉时间直接作为音频位置。
            targetTime = targetVisualTime - visualOffset;
        } else {
            // 没有 BPM 网格时仍允许定位，退化为按秒移动而不是忽略滚轮。
            double step = 0.25;
            // 无网格时加速使用用户设置的倍数，不把整拍模式当成固定秒数。
            if ( isShiftAccelerated )
                step *= m_ctx.lastConfig.settings.scrollSpeedMultiplier;
            targetTime = m_ctx.currentTime - static_cast<double>(wheel) * step;
        }
    } else {
        // 自由滚动保留滚轮连续增量；加速倍数只改变步长，不改变方向约定。
        double step = 0.25;
        if ( isShiftAccelerated )
            step *= m_ctx.lastConfig.settings.scrollSpeedMultiplier;
        targetTime = m_ctx.currentTime - static_cast<double>(wheel) * step;
    }

    double totalTime = SessionUtils::getEffectiveTotalTimeSeconds(m_ctx);
    double minTime   = -m_ctx.lastConfig.visual.getEffectiveVisualOffset();

    if ( minTime > totalTime ) {
        // 极端视觉偏移可让可见起点超出曲尾，此时范围收缩为曲尾单点。
        minTime = totalTime;
    }

    // 无论吸附还是自由滚动，都经过同一谱面范围约束再驱动音频。
    m_ctx.currentTime = std::clamp(targetTime, minTime, totalTime);
    if ( m_ctx.isActiveSession ) {
        auto& audio = Audio::AudioManager::instance();
        if ( m_ctx.isAudioTimelineActivationPending ||
             !audio.hasLoadedAudioTimeline() ||
             audio.getLoadedAudioTimelineFingerprint() !=
                 m_ctx.audioTimelineDescriptor.m_fingerprint ) {
            // 音频资源身份不匹配时走激活流程，以当前会话时间作为新定位点。
            (void)SessionUtils::activateAudioTimeline(m_ctx, m_ctx.isPlaying);
        } else {
            // 滚轮是离散定位，不使用拖动中的合并模式。
            audio.seek(m_ctx.currentTime);
            // 视觉定位立即生效，不依赖下一次音频快照到达。
            m_ctx.playbackVisualClock.rebase(m_ctx.currentTime,
                                             currentSteadySeconds(),
                                             audio.getPlaybackSpeed(),
                                             m_ctx.isPlaying);
        }
    }
    // 时间不连续变化后重新建立命中边界，长条持续效果不能只等下一次头部命中。
    SessionUtils::syncHitIndex(m_ctx);
    m_ctx.hitFXSystem.clearActiveEffects();
    restoreActiveHoldEffectsAfterPlaybackJump(m_ctx);
}

/// @brief 处理主画布中键二维平移。
/// @param cmd 逻辑像素空间中的平移增量和输入视口尺寸。
/// @details 横向位移保存在指定相机，纵向位移转换为会话时间并复用定位入口。
/// @note 纵向使用视觉时间与滚速缓存；传输时间不直接按像素线性相加。
/// @warning 中键拖动期间可每个 update
/// 调用；纵向输入还会进入定位与特效恢复流程。
/// 不得在此增加阻塞等待、文件操作或额外全量扫描。
void PlaybackController::handleCommand(const CmdPanCanvas& cmd)
{
    // 预览和时间线有各自的坐标与交互规则，不套用主画布的二维拖动解释。
    if ( !SessionUtils::isMainCanvasCameraId(cmd.cameraId) ) {
        return;
    }

    auto cameraIt = m_ctx.cameras.find(cmd.cameraId);
    if ( cameraIt == m_ctx.cameras.end() ) {
        // 首条输入可能早于相机注册，但必须有有效视口尺寸才能建立相机。
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
    // 已有相机继续保留自己的交互状态，仅按本次输入更新有效的视口信息。

    auto& camera = cameraIt->second;
    // 视口宽度变化时先换算已有横向偏移，再累加本次拖动，避免布局变化突跳。
    if ( std::isfinite(cmd.viewportWidth) && cmd.viewportWidth > 0.0F &&
         std::abs(camera.viewportWidth - cmd.viewportWidth) > 0.01F ) {
        // 尺寸容差过滤布局测量的浮点抖动，避免反复重算同一个横向偏移。
        camera.horizontalOffsetX = resizeCanvasHorizontalOffset(
            camera.horizontalOffsetX, camera.viewportWidth, cmd.viewportWidth);
        camera.viewportWidth = cmd.viewportWidth;
    }
    if ( std::isfinite(cmd.viewportHeight) && cmd.viewportHeight > 0.0F ) {
        // 无效尺寸不覆盖已有视口，防止窗口折叠时的输入污染后续相机换算。
        camera.viewportHeight = cmd.viewportHeight;
    }

    if ( std::isfinite(cmd.deltaX) ) {
        // 横向平移不改变谱面时间；累加溢出时仅恢复该相机的横向原点。
        camera.horizontalOffsetX += cmd.deltaX;
        if ( !std::isfinite(camera.horizontalOffsetX) ) {
            camera.horizontalOffsetX = 0.0F;
        }
    }

    if ( !std::isfinite(cmd.deltaY) || std::abs(cmd.deltaY) <= 0.001F ) {
        // 纯横向输入已经处理完毕，不要额外触发音频 seek 或命中特效重建。
        return;
    }

    const double visualOffset =
        m_ctx.lastConfig.visual.getEffectiveVisualOffset();
    // 判定线的视觉偏移在映射前加、定位前减，保持两种时间域对称。
    const double visualTime   = m_ctx.currentTime + visualOffset;
    double       renderScaleY = static_cast<double>(cmd.renderScaleY);
    if ( !std::isfinite(renderScaleY) || std::abs(renderScaleY) <= 1e-6 ) {
        // 缩放用于除法，未初始化或接近零时采用单位比例避免放大噪声。
        renderScaleY = 1.0;
    }
    // 有效负缩放仍保留符号，像素拖动方向应与实际渲染方向一致。

    double      targetVisualTime = visualTime;
    const auto* cache =
        m_ctx.timelineRegistry.ctx().find<System::ScrollCache>();
    // 只借用会话内已有缓存，本次换算不复制缓存或取得跨线程共享所有权。
    if ( cache ) {
        // 滚速事件使像素与时间不再线性，必须先移动绝对纵坐标再反查时间。
        // 增量除以渲染缩放后才属于缓存所使用的未缩放坐标空间。
        const double currentAbsY = cache->getVisualAnchorAbsY(visualTime);
        targetVisualTime         = cache->getTime(
            currentAbsY + static_cast<double>(cmd.deltaY) / renderScaleY);
    } else {
        // 缓存尚未就绪时提供固定比例的可用平移，不在输入路径临时重建缓存。
        constexpr double FALLBACK_PIXELS_PER_SECOND = 500.0;
        // 后备比例仅用于缓存缺失阶段，不写回滚速配置或改变谱面事件。
        targetVisualTime += static_cast<double>(cmd.deltaY) / renderScaleY /
                            FALLBACK_PIXELS_PER_SECOND;
    }

    if ( !std::isfinite(targetVisualTime) ) {
        // 反向映射无有效结果时保留原时间，已完成的横向平移不受影响。
        return;
    }

    // 复用统一定位入口处理范围、暂停偏好及音频归属；传入前转回谱面时间。
    // 未设置连续 scrub 标记，中键纵移采用定位命令默认的提交语义。
    handleCommand(CmdSeek{ targetVisualTime - visualOffset });

    // 直接操作期间不叠加滚动动画，否则视觉内容会落后于中键指针。
    // 使用定位后已钳制的会话时间，而不是可能超出曲尾的原始拖动目标。
    m_ctx.animateTime = m_ctx.currentTime + visualOffset;
    // 当前值和目标值一起对齐，关闭动画后也不遗留下次可恢复的旧目标。
    m_ctx.animateTimeTarget          = m_ctx.animateTime;
    m_ctx.animateTimeAnimationActive = false;
}


}  // namespace MMM::Logic
