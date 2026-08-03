#include "logic/ecs/system/HitFXSystem.h"
#include "audio/AudioManager.h"
#include "config/skin/SkinConfig.h"
#include "logic/ecs/system/render/Batcher.h"
#include <algorithm>
#include <cmath>

namespace MMM::Logic::System
{

bool HitFXSystem::isNonHoldEffectFinished(double elapsed,
                                          float  duration) noexcept
{
    if ( !std::isfinite(elapsed) || !std::isfinite(duration) ) return true;
    if ( elapsed < 0.0 ) return false;
    return elapsed >= static_cast<double>(std::max(duration, 0.0F));
}

std::optional<std::size_t> HitFXSystem::loopingEffectFrameIndex(
    double elapsed, float baseFps, std::size_t frameCount) noexcept
{
    if ( !std::isfinite(elapsed) || elapsed < 0.0 || !std::isfinite(baseFps) ||
         baseFps <= 0.0F || frameCount == 0U ) {
        return std::nullopt;
    }

    const double absoluteFrame = std::floor(elapsed * baseFps);
    if ( !std::isfinite(absoluteFrame) ) return std::nullopt;
    const double wrappedFrame =
        std::fmod(absoluteFrame, static_cast<double>(frameCount));
    return static_cast<std::size_t>(wrappedFrame);
}

void HitFXSystem::triggerAudio(const HitEvent& ev, std::int32_t trackCount,
                               const Config::EditorConfig& config)
{
    auto& audioManager = Audio::AudioManager::instance();

    // 1. 根据策略确定最终播放类型
    ::MMM::NoteType effectiveType = ev.type;

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

    // 2. 播放音效 (使用预定播放接口)
    float volumeFactor = 1.0f;
    if ( effectiveType == ::MMM::NoteType::FLICK &&
         config.settings.sfxConfig.enableFlickWidthVolumeScaling ) {
        volumeFactor =
            1.0f + (ev.trackSpan - 1) *
                       config.settings.sfxConfig.flickWidthVolumeMultiplier;
    }
    volumeFactor *= sampleVolumeForEvent(ev);

    const std::string& sfxKey = soundEffectKeyForEvent(ev, effectiveType);

    const auto stereoEnvelope = stereoGainEnvelopeForEvent(
        ev, trackCount, config.settings.sfxConfig.enableStereoHitEffects);
    const auto playbackControl = Audio::KeySoundPlaybackControl{
        .enabled          = true,
        .playerTrackIndex = ev.trackIndex >= 0
                                ? static_cast<std::uint32_t>(ev.trackIndex)
                                : Audio::KEY_SOUND_INVALID_TRACK_INDEX,
        .effectGroup      = hasBoundSoundEffect(ev)
                                ? Audio::KeySoundEffectGroup::Bound
                                : Audio::KeySoundEffectGroup::Unbound,
    };
    audioManager.playSoundEffectScheduled(
        sfxKey, ev.timestamp, volumeFactor, stereoEnvelope, playbackControl);
}

const std::string& HitFXSystem::soundEffectKeyForEvent(
    const HitEvent& ev, ::MMM::NoteType effectiveType)
{
    if ( hasBoundSoundEffect(ev) ) {
        return ev.sampleBinding->m_audioResourceId;
    }

    static const std::string NOTE_SOUND_EFFECT_KEY  = "hiteffect.note";
    static const std::string FLICK_SOUND_EFFECT_KEY = "hiteffect.flick";
    return effectiveType == ::MMM::NoteType::FLICK ? FLICK_SOUND_EFFECT_KEY
                                                   : NOTE_SOUND_EFFECT_KEY;
}

bool HitFXSystem::hasBoundSoundEffect(const HitEvent& ev) noexcept
{
    return ev.sampleBinding && !ev.sampleBinding->m_audioResourceId.empty();
}

float HitFXSystem::sampleVolumeForEvent(const HitEvent& ev)
{
    return hasBoundSoundEffect(ev) ? ev.sampleBinding->m_volume : 1.0F;
}

HitEffectRenderBounds HitFXSystem::calculateRenderBounds(
    Config::HitEffectLayoutMode layoutMode, std::int32_t trackCount,
    std::int32_t trackIndex, std::int32_t trackOffset, float judgmentLineY,
    float leftX, float topY, float bottomY, float singleTrackWidth,
    float fixedWidth, float fixedHeight)
{
    const std::int32_t lastTrack = std::max(trackCount - 1, 0);
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

Audio::StereoGainEnvelope HitFXSystem::stereoGainEnvelopeForEvent(
    const HitEvent& ev, std::int32_t trackCount, bool enabled)
{
    if ( !enabled || trackCount <= 0 ) return {};

    // 轨道索引从画面左侧向右递增，因此左声道增益应随索引增大而减小。
    // 右声道始终使用其补数，确保启用后两侧增益之和为 1。
    const auto leftGainAtTrack = [trackCount](int trackIndex) {
        const int boundedTrack =
            std::clamp(trackIndex, 0, static_cast<int>(trackCount) - 1);
        const float rightPosition = (static_cast<float>(boundedTrack) + 0.5F) /
                                    static_cast<float>(trackCount);
        return 1.0F - rightPosition;
    };

    const float startLeft = leftGainAtTrack(ev.trackIndex);
    const float endLeft = ev.type == ::MMM::NoteType::FLICK
                              ? leftGainAtTrack(ev.trackIndex + ev.trackOffset)
                              : startLeft;
    return {
        .startLeft  = startLeft,
        .startRight = 1.0F - startLeft,
        .endLeft    = endLeft,
        .endRight   = 1.0F - endLeft,
    };
}

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
    newEffect.trackSpan    = ev.trackSpan;
    newEffect.trackOffset  = ev.trackOffset;
    newEffect.isHold       = (ev.type == ::MMM::NoteType::HOLD);
    newEffect.effectKey    = effectKey;

    m_trackActiveEffects[ev.trackIndex] = newEffect;
}

std::size_t HitFXSystem::restoreActiveHoldEffects(
    double animateTime, std::span<const HitEvent> events,
    const Config::EditorConfig& config)
{
    if ( !config.visual.enableHitEffects || !std::isfinite(animateTime) ) {
        return 0U;
    }

    std::size_t restoredCount = 0U;
    for ( const auto& event : events ) {
        if ( !std::isfinite(event.timestamp) ) continue;
        if ( event.timestamp > animateTime ) break;
        if ( event.type != ::MMM::NoteType::HOLD ||
             !std::isfinite(event.duration) || event.duration < 0.0 ) {
            continue;
        }

        const double endTime = event.timestamp + event.duration;
        if ( !std::isfinite(endTime) || endTime < animateTime ) continue;
        triggerVisual(event, config);
        ++restoredCount;
    }
    return restoredCount;
}

/// @brief 更新打击特效状态。
/// @warning 逻辑热路径：每个 Session update
/// 执行；只处理本帧事件和当前活跃特效表。
void HitFXSystem::update(double                       animateTime,
                         const std::vector<HitEvent>& events,
                         std::int32_t                 trackCount,
                         const Config::EditorConfig&  config)
{
    if ( config.visual.canvasComponents.kps.visible ) {
        updateKps(animateTime, events, trackCount);
    } else if ( !m_recentHitEvents.empty() || !m_trackKps.empty() ) {
        clearKps();
        m_trackKps.clear();
    }

    for ( const auto& ev : events ) {
        triggerVisual(ev, config);
    }

    // 4. 清理已经播放完成的特效
    for ( auto it = m_trackActiveEffects.begin();
          it != m_trackActiveEffects.end(); ) {
        auto& active = it->second;

        if ( active.isHold ) {
            auto&       skinManager = Config::SkinManager::instance();
            const auto* seq = skinManager.getEffectSequence("note.effect." +
                                                            active.effectKey);
            const std::size_t frameCount = seq ? seq->frames.size() : 0U;
            const float       baseFps    = skinManager.getEffectBaseFps();
            // 对于 Hold，如果当前时间超过了 Hold
            // 结束时间，且至少播放完一个完整的普通动画周期，则结束
            // 这确保了极短或 0 时长的 Hold 也能正常播放完一个完整的打击动画
            double animDuration = static_cast<double>(frameCount) / baseFps;
            if ( animateTime > (active.startTime + active.holdDuration) &&
                 animateTime >= (active.startTime + animDuration) ) {
                it = m_trackActiveEffects.erase(it);
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

void HitFXSystem::updateKps(double                       animateTime,
                            const std::vector<HitEvent>& events,
                            std::int32_t                 trackCount)
{
    const auto safeTrackCount =
        static_cast<std::size_t>(std::max<std::int32_t>(trackCount, 0));
    if ( m_trackKps.size() != safeTrackCount ) {
        m_recentHitEvents.clear();
        m_trackKps.assign(safeTrackCount, 0U);
    }

    if ( !std::isfinite(animateTime) ||
         (m_lastKpsTime >= 0.0 && animateTime < m_lastKpsTime) ) {
        clearKps();
    }
    m_lastKpsTime = std::isfinite(animateTime) ? animateTime : -1.0;
    if ( !std::isfinite(animateTime) ) return;

    for ( const auto& event : events ) {
        const auto hitTrack = static_cast<std::int64_t>(event.trackIndex) +
                              static_cast<std::int64_t>(event.trackOffset);
        if ( !std::isfinite(event.timestamp) || hitTrack < 0 ||
             static_cast<std::size_t>(hitTrack) >= m_trackKps.size() ) {
            continue;
        }
        m_recentHitEvents.push_back(
            { event.timestamp, static_cast<std::int32_t>(hitTrack) });
        ++m_trackKps[static_cast<std::size_t>(hitTrack)];
    }

    const double windowStart = animateTime - 1.0;
    while ( !m_recentHitEvents.empty() &&
            m_recentHitEvents.front().timestamp <= windowStart ) {
        const auto trackIndex = m_recentHitEvents.front().trackIndex;
        if ( trackIndex >= 0 &&
             static_cast<std::size_t>(trackIndex) < m_trackKps.size() ) {
            auto& count = m_trackKps[static_cast<std::size_t>(trackIndex)];
            if ( count > 0U ) --count;
        }
        m_recentHitEvents.pop_front();
    }
}

void HitFXSystem::clearKps()
{
    m_recentHitEvents.clear();
    std::fill(m_trackKps.begin(), m_trackKps.end(), 0U);
    m_lastKpsTime = -1.0;
}

void HitFXSystem::clearActiveEffects()
{
    m_trackActiveEffects.clear();
    clearKps();
}

/// @brief 生成打击特效的渲染指令。
/// @warning 渲染热路径：快照生成阶段执行；只遍历当前活跃特效表并追加几何。
void HitFXSystem::generateSnapshot(Batcher& batcher, double animateTime,
                                   const Config::EditorConfig& config,
                                   int32_t trackCount, float judgmentLineY,
                                   float leftX, float topY, float bottomY,
                                   float singleTrackW)
{
    if ( !config.visual.enableHitEffects || m_trackActiveEffects.empty() ||
         trackCount <= 0 || singleTrackW <= 0.0f )
        return;

    RenderSnapshot* snapshot    = batcher.snapshot;
    auto&           skinManager = Config::SkinManager::instance();
    float           baseFps     = skinManager.getEffectBaseFps();
    const auto      layoutMode  = skinManager.getHitEffectLayoutMode();

    for ( const auto& [track, active] : m_trackActiveEffects ) {
        const auto* seq =
            skinManager.getEffectSequence("note.effect." + active.effectKey);
        if ( !seq || seq->frames.empty() ) continue;

        size_t frameCount = seq->frames.size();
        double elapsed    = animateTime - active.startTime;
        if ( elapsed < 0 )
            continue;  // 尚未到达触发点（虽然 logic 逻辑应该保证触发）

        if ( !active.isHold &&
             isNonHoldEffectFinished(elapsed,
                                     config.visual.nonHoldHitEffectDuration) ) {
            continue;
        }
        const auto frameIndex =
            loopingEffectFrameIndex(elapsed, baseFps, frameCount);
        if ( !frameIndex ) continue;

        // 使用 SkinManager 统一分配好的起始 ID
        uint32_t textureId = seq->startId + static_cast<uint32_t>(*frameIndex);

        // 获取特效序列帧的 UV 信息以计算比例
        auto itTex = snapshot->uvMap.find(textureId);
        if ( itTex == snapshot->uvMap.end() || itTex->second.z <= 0.0f ||
             itTex->second.w <= 0.0f )
            continue;
        float texAspect = itTex->second.z / itTex->second.w;

        // 固定模式继续复用物件横纵缩放；整轨模式只使用其纵横比采样纹理。
        const float fixedWidth = singleTrackW * config.visual.noteScaleX;
        const float fixedHeight =
            (singleTrackW / texAspect) * config.visual.noteScaleY;
        const HitEffectRenderBounds bounds =
            calculateRenderBounds(layoutMode,
                                  trackCount,
                                  active.trackIndex,
                                  active.trackOffset,
                                  judgmentLineY,
                                  leftX,
                                  topY,
                                  bottomY,
                                  singleTrackW,
                                  fixedWidth,
                                  fixedHeight);

        batcher.setTexture(static_cast<TextureID>(textureId));
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

    batcher.flush();
}

}  // namespace MMM::Logic::System
