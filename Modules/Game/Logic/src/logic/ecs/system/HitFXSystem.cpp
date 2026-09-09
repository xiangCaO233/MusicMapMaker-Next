#include "logic/ecs/system/HitFXSystem.h"
#include "audio/AudioManager.h"
#include "config/skin/SkinConfig.h"
#include "logic/ecs/system/render/Batcher.h"
#include <algorithm>
#include <cmath>

namespace MMM::Logic::System
{

/// @brief 判断非持续型特效是否达到结束时刻。
/// @param elapsed 相对触发时间的经过秒数，允许尚未到达触发时刻。
/// @param duration 皮肤定义的播放时长，负值按零处理。
/// @return 非有限输入直接结束；负 elapsed 保留等待触发的特效。
/// @note 结束边界使用大于等于，零寿命在触发时刻即可结束。
/// @warning 特效更新热路径，仅执行数值判断。
bool HitFXSystem::isNonHoldEffectFinished(double elapsed,
                                          float  duration) noexcept
{
    if ( !std::isfinite(elapsed) || !std::isfinite(duration) ) return true;
    if ( elapsed < 0.0 ) return false;
    return elapsed >= static_cast<double>(std::max(duration, 0.0F));
}

/// @brief 将循环播放时长换算为皮肤动画帧索引。
/// @param elapsed 从特效起点经过的秒数。
/// @param baseFps 皮肤基础帧率，须为有限正数。
/// @param frameCount 可用帧数量，零帧没有合法索引。
/// @return 非法输入或帧数计算溢出时为空。
/// @note 返回零表示合法的首帧，调用方须检查 optional 而不是索引数值。
/// @note 使用 floor 选取当前帧，不在相邻动画帧之间混合图像。
/// @warning 热路径常量计算，不以循环逐帧追赶时间。
std::optional<std::size_t> HitFXSystem::loopingEffectFrameIndex(
    double elapsed, float baseFps, std::size_t frameCount) noexcept
{
    if ( !std::isfinite(elapsed) || elapsed < 0.0 || !std::isfinite(baseFps) ||
         baseFps <= 0.0F || frameCount == 0U ) {
        return std::nullopt;
    }

    const double absoluteFrame = std::floor(elapsed * baseFps);
    // 先在浮点域取模，再转索引，避免长时间播放产生过大的整数转换。
    if ( !std::isfinite(absoluteFrame) ) return std::nullopt;
    const double wrappedFrame =
        std::fmod(absoluteFrame, static_cast<double>(frameCount));
    return static_cast<std::size_t>(wrappedFrame);
}

/// @brief 按命中事件计划播放键音，并附带区域、声像和绑定分组信息。
/// @param ev 谱面时间线上的命中事件，实际播放时刻使用其 timestamp。
/// @param playerTrackCount 玩家区轨道数。
/// @param draftTrackCount 草稿区轨道数，负数时沿用兼容回退规则。
/// @param config 当前折线键音、滑键音量及立体声配置。
/// @note 本函数不登记视觉特效，预调度声音不应提前显示命中动画。
/// @note 滑键宽度音量与绑定音量相乘，不改变原事件中存储的倍率。
/// @warning 播放调度路径调用；不得在这里等待设备时间或同步解码音频。
void HitFXSystem::triggerAudio(const HitEvent&             ev,
                               std::int32_t                playerTrackCount,
                               std::int32_t                draftTrackCount,
                               const Config::EditorConfig& config)
{
    auto& audioManager = Audio::AudioManager::instance();

    // 1. 根据策略确定最终播放类型
    ::MMM::NoteType effectiveType = ev.type;

    if ( ev.isSubNote ) {
        // 策略只替换播放类型，不改变原事件的轨道跨度和绑定资源。
        const auto& strategy = config.settings.sfxConfig.polylineStrategy;
        switch ( strategy ) {
        case Config::PolylineSfxStrategy::Exact: break;
        case Config::PolylineSfxStrategy::InternalAsNormal:
            if ( ev.role == HitEvent::Role::Internal )
                effectiveType = ::MMM::NoteType::NOTE;
            break;
        case Config::PolylineSfxStrategy::OnlyTailExact:
            if ( ev.role != HitEvent::Role::Tail )
                effectiveType = ::MMM::NoteType::NOTE;
            break;
        case Config::PolylineSfxStrategy::AllAsNormal:
            effectiveType = ::MMM::NoteType::NOTE;
            break;
        }
    }

    // 2. 播放音效 (使用预定播放接口)
    float volumeFactor = 1.0f;
    // 宽度增益依据策略处理后的类型，改作普通点按的折线节点不再按滑键放大。
    if ( effectiveType == ::MMM::NoteType::FLICK &&
         config.settings.sfxConfig.enableFlickWidthVolumeScaling ) {
        volumeFactor =
            1.0f + (ev.trackSpan - 1) *
                       config.settings.sfxConfig.flickWidthVolumeMultiplier;
    }
    volumeFactor *= sampleVolumeForEvent(ev);
    // 绑定音量作为乘数合入；未绑定事件保持默认单位音量。

    const std::string& sfxKey = soundEffectKeyForEvent(ev, effectiveType);

    const auto stereoEnvelope = stereoGainEnvelopeForEvent(
        // 声像仍按真实事件运动计算，不随键音类型替换而丢失滑动方向。
        ev,
        playerTrackCount,
        config.settings.sfxConfig.enableStereoHitEffects,
        draftTrackCount);
    const auto areaTrack =
        areaTrackIndexForEvent(ev, playerTrackCount, draftTrackCount);
    const auto playbackControl = Audio::KeySoundPlaybackControl{
        // 区域和绑定分组供音频侧控制使用，不把草稿轨道当成玩家区索引。
        .enabled          = true,
        .area             = ev.isDraft ? Audio::KeySoundPlaybackArea::Draft
                                       : Audio::KeySoundPlaybackArea::Player,
        .playerTrackIndex = areaTrack >= 0
                                ? static_cast<std::uint32_t>(areaTrack)
                                : Audio::KEY_SOUND_INVALID_TRACK_INDEX,
        .effectGroup      = hasBoundSoundEffect(ev)
                                ? Audio::KeySoundEffectGroup::Bound
                                : Audio::KeySoundEffectGroup::Unbound,
    };
    audioManager.playSoundEffectScheduled(
        sfxKey, ev.timestamp, volumeFactor, stereoEnvelope, playbackControl);
}

/// @brief 选择绑定资源或默认键音资源键。
/// @param ev 提供可选采样绑定的事件。
/// @param effectiveType 已经过折线键音策略转换的播放类型。
/// @return 绑定字符串或静态默认键的借用引用，不返回临时字符串。
/// @note 绑定引用的有效期受 ev 及其绑定对象限制，不能缓存到事件销毁之后。
/// @warning 调度热路径只选择现有键，不复制资源 ID。
const std::string& HitFXSystem::soundEffectKeyForEvent(
    const HitEvent& ev, ::MMM::NoteType effectiveType)
{
    if ( hasBoundSoundEffect(ev) ) {
        // 显式绑定优先于默认 note/flick 类型选择。
        return ev.sampleBinding->m_audioResourceId;
    }

    static const std::string NOTE_SOUND_EFFECT_KEY  = "hiteffect.note";
    static const std::string FLICK_SOUND_EFFECT_KEY = "hiteffect.flick";
    return effectiveType == ::MMM::NoteType::FLICK ? FLICK_SOUND_EFFECT_KEY
                                                   : NOTE_SOUND_EFFECT_KEY;
}

/// @brief 检查是否存在非空采样资源绑定。
/// @note 存在绑定结构但资源 ID 为空时仍按未绑定处理。
/// @return 只表示具有资源标识，不保证对应声音可以解码播放。
/// @warning 热路径常量查询，不查资源是否实际加载。
bool HitFXSystem::hasBoundSoundEffect(const HitEvent& ev) noexcept
{
    return ev.sampleBinding && !ev.sampleBinding->m_audioResourceId.empty();
}

/// @brief 返回有效绑定的音量倍率，否则使用单位音量。
/// @param ev 原始事件，音量不受折线类型替换策略影响。
/// @warning 调度热路径直接读取已准备的绑定，不重新规范化音量。
float HitFXSystem::sampleVolumeForEvent(const HitEvent& ev)
{
    return hasBoundSoundEffect(ev) ? ev.sampleBinding->m_volume : 1.0F;
}

/// @brief 按整轨铺满或固定尺寸模式计算特效绘制框。
/// @param layoutMode 整轨模式忽略固定宽高，其余模式使用固定框。
/// @param trackCount 当前区域轨道数量；零轨道保留兼容的零号索引。
/// @param trackIndex 起始轨道，使用传入区域的局部索引。
/// @param trackOffset 相对起始轨道的目标偏移。
/// @param judgmentLineY 固定尺寸特效的纵向中心。
/// @param leftX 当前轨道区域左边界。
/// @param topY 整轨模式的上边界。
/// @param bottomY 整轨模式的下边界，也是返回框的底边。
/// @param singleTrackWidth 当前区域单轨宽度。
/// @param fixedWidth 固定模式的绘制宽度。
/// @param fixedHeight 固定模式的绘制高度。
/// @return 使用底边 Y 约定的矩形，可直接传给 Batcher 填充入口。
/// @pre 输入为有限坐标，轨道与偏移之和处于索引类型可表示范围内。
/// @warning 渲染热路径仅计算矩形，不生成顶点或改变裁剪状态。
HitEffectRenderBounds HitFXSystem::calculateRenderBounds(
    Config::HitEffectLayoutMode layoutMode, std::int32_t trackCount,
    std::int32_t trackIndex, std::int32_t trackOffset, float judgmentLineY,
    float leftX, float topY, float bottomY, float singleTrackWidth,
    float fixedWidth, float fixedHeight)
{
    const std::int32_t lastTrack = std::max(trackCount - 1, 0);
    // 终点夹在区域有效索引内，跨轨滑键不会把特效框放到区域外。
    const std::int32_t targetTrack =
        std::clamp(trackIndex + trackOffset, 0, lastTrack);

    if ( layoutMode == Config::HitEffectLayoutMode::TrackFill ) {
        return {
            .x     = leftX + static_cast<float>(targetTrack) * singleTrackWidth,
            .y     = bottomY,
            .width = singleTrackWidth,
            .height = std::max(0.0f, bottomY - topY),
        };
    }

    const float centerX =
        leftX + (static_cast<float>(targetTrack) + 0.5f) * singleTrackWidth;
    return {
        .x      = centerX - fixedWidth * 0.5f,
        .y      = judgmentLineY + fixedHeight * 0.5f,
        .width  = fixedWidth,
        .height = fixedHeight,
    };
}

/// @brief 将草稿负轨道索引换算为其区域内从左到右的索引。
/// @param ev 提供区域类型及绝对轨道索引的命中事件。
/// @param playerTrackCount 兼容模式下用于推导草稿区域宽度。
/// @param draftTrackCount 显式草稿轨道数；负值表示沿用玩家轨道数。
/// @return 玩家轨道原样返回；草稿轨道加上区域轨道数，不在此夹紧。
/// @warning 调度热路径常量计算，无注册表访问。
std::int32_t HitFXSystem::areaTrackIndexForEvent(
    const HitEvent& ev, std::int32_t playerTrackCount,
    std::int32_t draftTrackCount) noexcept
{
    const auto effectiveDraftCount =
        draftTrackCount >= 0 ? draftTrackCount : playerTrackCount;
    return ev.isDraft ? ev.trackIndex + std::max(effectiveDraftCount, 0)
                      : ev.trackIndex;
}

/// @brief 按区域轨道中心计算左右声道起止增益。
/// @param ev 提供真实滑动方向与区域归属的事件。
/// @param playerTrackCount 玩家区域的轨道总数。
/// @param enabled 未开启时返回默认声像包络。
/// @param draftTrackCount 草稿区域独立轨道数，负值时采用玩家轨道数。
/// @return 每个端点的左右增益之和为 1，起止值供音频侧插值。
/// @note 这里生成声像包络，不决定包络实际播放时长或音频采样位置。
/// @note 滑键终点使用原事件偏移，其他物件保持起点声像。
/// @warning 调度热路径数值计算，不查询或修改音频设备状态。
Audio::StereoGainEnvelope HitFXSystem::stereoGainEnvelopeForEvent(
    const HitEvent& ev, std::int32_t playerTrackCount, bool enabled,
    std::int32_t draftTrackCount)
{
    const auto areaTrackCount =
        ev.isDraft && draftTrackCount >= 0 ? draftTrackCount : playerTrackCount;
    if ( !enabled || areaTrackCount <= 0 ) return {};

    // 草稿区与玩家区分别按各自从左到右的轨道中心计算声像。
    const auto leftGainAtTrack = [areaTrackCount](int trackIndex) {
        // 越界终点夹到最外侧轨道，避免计算出负增益或大于一的增益。
        // 使用轨道中心而不是边界，最外侧轨道也保留另一声道的少量增益。
        const int boundedTrack =
            std::clamp(trackIndex, 0, static_cast<int>(areaTrackCount) - 1);
        const float rightPosition = (static_cast<float>(boundedTrack) + 0.5F) /
                                    static_cast<float>(areaTrackCount);
        return 1.0F - rightPosition;
    };

    const int areaTrack =
        areaTrackIndexForEvent(ev, playerTrackCount, draftTrackCount);
    const float startLeft = leftGainAtTrack(areaTrack);
    const float endLeft   = ev.type == ::MMM::NoteType::FLICK
                                ? leftGainAtTrack(areaTrack + ev.trackOffset)
                                : startLeft;
    return {
        .startLeft  = startLeft,
        .startRight = 1.0F - startLeft,
        .endLeft    = endLeft,
        .endRight   = 1.0F - endLeft,
    };
}

/// @brief 建立命中视觉特效，同轨道的新事件替换旧事件。
/// @param ev 提供原始物件类型、轨道和持续时间的事件。
/// @param config 决定特效开关和折线节点的表现类型。
/// @note 新实例以事件时间为起点，不使用触发函数被调用时的墙钟时间。
/// @note 关闭特效时不新增实例，已有实例的清理仍由更新或显式重置完成。
/// @warning 事件触发路径更新活动表，不在此加载皮肤动画。
void HitFXSystem::triggerVisual(const HitEvent&             ev,
                                const Config::EditorConfig& config)
{
    if ( !config.visual.enableHitEffects ) return;

    ::MMM::NoteType effectiveType = ev.type;
    std::string     effectKey     = "note";

    if ( ev.isSubNote ) {
        const auto& strategy = config.settings.sfxConfig.polylineStrategy;
        switch ( strategy ) {
        case Config::PolylineSfxStrategy::Exact: break;
        case Config::PolylineSfxStrategy::InternalAsNormal:
            if ( ev.role == HitEvent::Role::Internal )
                effectiveType = ::MMM::NoteType::NOTE;
            break;
        case Config::PolylineSfxStrategy::OnlyTailExact:
            if ( ev.role != HitEvent::Role::Tail )
                effectiveType = ::MMM::NoteType::NOTE;
            break;
        case Config::PolylineSfxStrategy::AllAsNormal:
            effectiveType = ::MMM::NoteType::NOTE;
            break;
        }
    }

    if ( effectiveType == ::MMM::NoteType::FLICK ) {
        effectKey = "flick";
    }

    ActiveEffect newEffect;
    newEffect.startTime    = ev.timestamp;
    newEffect.holdDuration = ev.duration;
    newEffect.trackIndex   = ev.trackIndex;
    newEffect.isDraft      = ev.isDraft;
    newEffect.trackSpan    = ev.trackSpan;
    newEffect.trackOffset  = ev.trackOffset;
    newEffect.isHold       = (ev.type == ::MMM::NoteType::HOLD);
    // 持续生命周期仍按原始物件类型决定，不因表现策略改成普通键音而缩短。
    newEffect.effectKey = effectKey;

    m_trackActiveEffects[ev.trackIndex] = newEffect;
    // 每个绝对轨道只保留最新特效，避免快速连续命中无限叠加实例。
}

/// @brief 跳转后恢复覆盖当前时刻的长条视觉特效，不重放声音。
/// @param animateTime 要恢复的动画时间，单位秒。
/// @param events 按 timestamp 升序排列的命中事件。
/// @param config 当前特效开关与节点表现策略。
/// @return 触发恢复的事件数量，不保证等于最终活动轨道数量。
/// @note 不先清除现有活动表，调用方需在跳转重置流程中安排清理。
/// @warning 播放跳转的低频分支遍历历史事件，不得每次 update 无条件恢复。
std::size_t HitFXSystem::restoreActiveHoldEffects(
    double animateTime, std::span<const HitEvent> events,
    const Config::EditorConfig& config)
{
    if ( !config.visual.enableHitEffects || !std::isfinite(animateTime) ) {
        return 0U;
    }

    std::size_t restoredCount = 0U;
    for ( const auto& event : events ) {
        // 利用时间有序性遇到未来事件即可停止；非有限时间点不能参与覆盖判断。
        if ( !std::isfinite(event.timestamp) ) continue;
        if ( event.timestamp > animateTime ) break;
        if (
            event.type != ::MMM::NoteType::HOLD ||
            // 已结束的一次性特效不因跳转重新播一遍，只恢复仍覆盖当前时间的长条。
            !std::isfinite(event.duration) || event.duration < 0.0 ) {
            continue;
        }

        const double endTime = event.timestamp + event.duration;
        // 长条结束时刻恰好等于当前时刻仍可恢复，后续寿命判定负责最终移除。
        if ( !std::isfinite(endTime) || endTime < animateTime ) continue;
        triggerVisual(event, config);
        // 同轨道后出现的命中会覆盖前者，保持正常播放时的最近事件规则。
        ++restoredCount;
    }
    return restoredCount;
}

/// @brief 更新打击特效状态。
/// @param animateTime 当前会话的动画时间。
/// @param events 本轮新到达判定线的事件，不是完整历史列表。
/// @param trackCount 玩家轨道数，供 KPS 统计使用。
/// @param config 特效寿命与 KPS 可见性配置。
/// @note KPS 和动画寿命都以 animateTime 为准，不能混用系统单调时间。
/// @note 关闭特效只阻止新增表现，不会阻断本轮 KPS 数据更新。
/// @warning 逻辑热路径：每个 Session update
/// 执行；只处理本帧事件和当前活跃特效表。
void HitFXSystem::update(double                       animateTime,
                         const std::vector<HitEvent>& events,
                         std::int32_t                 trackCount,
                         const Config::EditorConfig&  config)
{
    if ( config.visual.canvasComponents.kps.visible ) {
        // KPS 的可见性独立于特效开关，不能因关闭动画就停止键数统计。
        updateKps(animateTime, events, trackCount);
    } else if ( !m_recentHitEvents.empty() || !m_trackKps.empty() ) {
        // 隐藏后清除计数及轨道表，重新显示时从新的窗口开始累计。
        clearKps();
        m_trackKps.clear();
    }

    for ( const auto& ev : events ) {
        // 先接收本帧命中再清理寿命，极短特效仍按统一过期规则处理。
        triggerVisual(ev, config);
    }

    // 4. 清理已经播放完成的特效
    for ( auto it = m_trackActiveEffects.begin();
          it != m_trackActiveEffects.end(); ) {
        auto& active = it->second;

        if ( active.isHold ) {
            // 长条最短可见时长取皮肤一轮动画长度，持续阶段允许循环播放。
            auto&       skinManager = Config::SkinManager::instance();
            const auto* seq = skinManager.getEffectSequence("note.effect." +
                                                            active.effectKey);
            const std::size_t frameCount = seq ? seq->frames.size() : 0U;
            const float       baseFps    = skinManager.getEffectBaseFps();
            // 对于 Hold，如果当前时间超过了 Hold
            // 结束时间，且至少播放完一个完整的普通动画周期，则结束
            // 这确保了极短或 0 时长的 Hold 也能正常播放完一个完整的打击动画
            double animDuration = static_cast<double>(frameCount) / baseFps;
            // 两个结束条件必须同时成立：长条已过尾部，且最短动画周期已完成。
            if ( animateTime > (active.startTime + active.holdDuration) &&
                 animateTime >= (active.startTime + animDuration) ) {
                it = m_trackActiveEffects.erase(it);
                // erase 返回下一元素，不再递增已失效的旧迭代器。
                continue;
            }
        } else {
            // 普通物件服从视觉配置的固定寿命，与皮肤帧数解耦。
            if ( isNonHoldEffectFinished(
                     animateTime - active.startTime,
                     config.visual.nonHoldHitEffectDuration) ) {
                it = m_trackActiveEffects.erase(it);
                continue;
            }
        }
        ++it;
    }
}

/// @brief 增量维护最近一秒内各玩家轨道的命中计数。
/// @param animateTime 窗口右端时间，倒退或非法时清空历史。
/// @param events 本轮新增且按时间排序的事件，不应重复传入旧事件。
/// @param trackCount 玩家轨道数，非正值产生空统计表。
/// @pre events 应是当前已发生的事件，不能把预调度的未来音频事件传入此处。
/// @warning 逻辑热路径只追加新事件和弹出过期队首，不遍历谱面物件。
void HitFXSystem::updateKps(double                       animateTime,
                            const std::vector<HitEvent>& events,
                            std::int32_t                 trackCount)
{
    const auto safeTrackCount =
        static_cast<std::size_t>(std::max<std::int32_t>(trackCount, 0));
    if ( m_trackKps.size() != safeTrackCount ) {
        // 轨道布局改变后旧队列中的索引失去对应关系，必须与计数表一起重置。
        m_recentHitEvents.clear();
        m_trackKps.assign(safeTrackCount, 0U);
    }

    // 时间回退后不能让未来命中继续留在当前窗口内。
    if ( !std::isfinite(animateTime) ||
         (m_lastKpsTime >= 0.0 && animateTime < m_lastKpsTime) ) {
        clearKps();
    }
    m_lastKpsTime = std::isfinite(animateTime) ? animateTime : -1.0;
    if ( !std::isfinite(animateTime) ) return;

    for ( const auto& event : events ) {
        // 草稿命中不计入玩家 KPS，滑键则记到实际目标轨道。
        if ( event.isDraft ) continue;
        // 先提升位宽再加偏移，避免索引计算发生窄整数溢出。
        const auto hitTrack = static_cast<std::int64_t>(event.trackIndex) +
                              static_cast<std::int64_t>(event.trackOffset);
        if ( !std::isfinite(event.timestamp) || hitTrack < 0 ||
             static_cast<std::size_t>(hitTrack) >= m_trackKps.size() ) {
            continue;
        }
        m_recentHitEvents.push_back(
            // 队列保留过期时扣减所需的时间和轨道，不复制整个命中事件。
            { event.timestamp, static_cast<std::int32_t>(hitTrack) });
        ++m_trackKps[static_cast<std::size_t>(hitTrack)];
    }

    const double windowStart = animateTime - 1.0;
    // 左边界为开区间，恰好满一秒的命中也从统计中移除。
    while ( !m_recentHitEvents.empty() &&
            m_recentHitEvents.front().timestamp <= windowStart ) {
        const auto trackIndex = m_recentHitEvents.front().trackIndex;
        if ( trackIndex >= 0 &&
             static_cast<std::size_t>(trackIndex) < m_trackKps.size() ) {
            auto& count = m_trackKps[static_cast<std::size_t>(trackIndex)];
            if ( count > 0U ) --count;
            // 防止计数回退越过零，轨道计数与队列删除仍按同一事件推进。
        }
        m_recentHitEvents.pop_front();
    }
}

/// @brief 清空窗口历史和计数，保留现有轨道表容量。
/// @note 时间哨兵重置为 -1，下一次有效 update 重新建立时间基准。
/// @note 保留表长度，使同轨道布局下的跳转无需重新分配计数表。
/// @warning 跳转或配置切换时调用，不执行文件或音频操作。
void HitFXSystem::clearKps()
{
    m_recentHitEvents.clear();
    std::fill(m_trackKps.begin(), m_trackKps.end(), 0U);
    m_lastKpsTime = -1.0;
}

/// @brief 清除全部视觉特效及 KPS 历史。
/// @note 不取消已经调度的音频，声音队列由会话播放控制流程独立处理。
/// @note 无恢复事件时不会自行重建长条特效，恢复属于独立调用步骤。
/// @warning 播放跳转等状态切换时使用，不应逐帧重置活动状态。
void HitFXSystem::clearActiveEffects()
{
    m_trackActiveEffects.clear();
    clearKps();
}

/// @brief 生成打击特效的渲染指令。
/// @param batcher 当前快照的几何输出器，调用结束会提交特效批次。
/// @param animateTime 选择动画帧的时间基准，单位秒。
/// @param config 特效可见性、物件缩放和填充方式。
/// @param trackCount 本次绘制区域的轨道数，草稿区域需传草稿轨道数。
/// @param judgmentLineY 固定尺寸特效的中心纵坐标。
/// @param leftX 该区域的左边界。
/// @param topY 该区域的上边界。
/// @param bottomY 该区域的下边界。
/// @param singleTrackW 该区域单轨宽度。
/// @param renderDraftEffects 本次绘制草稿区还是玩家区。
/// @note 此函数修改批处理器裁剪状态，不自动恢复先前区域。
/// @pre batcher.snapshot 有效，皮肤序列和快照图集在生成期间保持稳定。
/// @note 快照生成不删除活动实例，缺纹理的特效仍由逻辑寿命规则回收。
/// @note 本次区域没有可画帧时也不补默认动画，避免缺失皮肤产生错误表现。
/// @warning 渲染热路径：快照生成阶段执行；只遍历当前活跃特效表并追加几何。
void HitFXSystem::generateSnapshot(Batcher& batcher, double animateTime,
                                   const Config::EditorConfig& config,
                                   int32_t trackCount, float judgmentLineY,
                                   float leftX, float topY, float bottomY,
                                   float singleTrackW, bool renderDraftEffects)
{
    if ( !config.visual.enableHitEffects || m_trackActiveEffects.empty() ||
         trackCount <= 0 || singleTrackW <= 0.0f )
        return;

    // 特效只能进入当前区域，固定尺寸图元也不能越界覆盖相邻区域。
    batcher.setScissor(leftX,
                       topY,
                       singleTrackW * static_cast<float>(trackCount),
                       bottomY - topY);

    RenderSnapshot* snapshot    = batcher.snapshot;
    auto&           skinManager = Config::SkinManager::instance();
    float           baseFps     = skinManager.getEffectBaseFps();
    const auto      layoutMode  = skinManager.getHitEffectLayoutMode();

    for ( const auto& [track, active] : m_trackActiveEffects ) {
        (void)track;
        if ( active.isDraft != renderDraftEffects ) continue;
        // 两个区域分别绘制，共享活动表但不重复提交同一特效。
        const auto* seq =
            skinManager.getEffectSequence("note.effect." + active.effectKey);
        if ( !seq || seq->frames.empty() ) continue;
        // 皮肤没有对应序列时跳过几何，不在快照生成阶段补载资源。

        size_t frameCount = seq->frames.size();
        double elapsed    = animateTime - active.startTime;
        if ( elapsed < 0 )
            continue;  // 尚未到达触发点（虽然 logic 逻辑应该保证触发）

        // 渲染侧再次判断寿命，避免更新与快照时间差让普通动画多显示一帧。
        if ( !active.isHold &&
             isNonHoldEffectFinished(elapsed,
                                     config.visual.nonHoldHitEffectDuration) ) {
            continue;
        }
        // 持续长条和仍存活的普通特效均循环取帧，结束条件由寿命规则决定。
        const auto frameIndex =
            loopingEffectFrameIndex(elapsed, baseFps, frameCount);
        if ( !frameIndex ) continue;

        // 使用 SkinManager 统一分配好的起始 ID
        uint32_t textureId = seq->startId + static_cast<uint32_t>(*frameIndex);

        // 获取特效序列帧的 UV 信息以计算比例
        auto itTex = snapshot->uvMap.find(textureId);
        // 帧 ID 存在不代表图集 UV 已准备，缺失或零尺寸均不生成图元。
        if ( itTex == snapshot->uvMap.end() || itTex->second.z <= 0.0f ||
             itTex->second.w <= 0.0f )
            continue;
        float texAspect = itTex->second.z / itTex->second.w;

        // 固定模式继续复用物件横纵缩放；整轨模式只使用其纵横比采样纹理。
        const float fixedWidth = singleTrackW * config.visual.noteScaleX;
        const float fixedHeight =
            (singleTrackW / texAspect) * config.visual.noteScaleY;
        // 草稿负索引转为本次区域内索引，玩家索引无需转换。
        const int renderTrack =
            active.isDraft ? active.trackIndex + trackCount : active.trackIndex;
        const HitEffectRenderBounds bounds =
            calculateRenderBounds(layoutMode,
                                  trackCount,
                                  renderTrack,
                                  active.trackOffset,
                                  judgmentLineY,
                                  leftX,
                                  topY,
                                  bottomY,
                                  singleTrackW,
                                  fixedWidth,
                                  fixedHeight);

        batcher.setTexture(static_cast<TextureID>(textureId));
        // 特效以纹理自身颜色显示，不叠加物件自定义颜色或音量可视化乘色。
        // 整轨模式必须完整拉伸；固定模式继续服从原有物件填充设置。
        const auto fillMode =
            layoutMode == Config::HitEffectLayoutMode::TrackFill
                ? Config::BackgroundFillMode::Stretch
                : config.visual.noteFillMode;
        batcher.pushFilledQuad(bounds.x,
                               bounds.y,
                               bounds.width,
                               bounds.height,
                               { texAspect, 1.0f },
                               fillMode,
                               glm::vec4(1.0f));
    }

    // 保证末尾同纹理图元形成完整命令，调用方随后可切换其他渲染职责。
    batcher.flush();
}

}  // namespace MMM::Logic::System
