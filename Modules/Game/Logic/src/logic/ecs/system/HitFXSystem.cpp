#include "logic/ecs/system/HitFXSystem.h"
#include "audio/AudioManager.h"
#include "config/skin/SkinConfig.h"
#include "logic/ecs/system/render/Batcher.h"
#include <cmath>

namespace MMM::Logic::System
{

bool HitFXSystem::ActiveEffect::isFinished(double currentTime, float baseFps,
                                           size_t frameCount) const
{
    if ( isLooping )
        return false;  // Hold 在持续时间内不视为完成（由 update 外部控制）
    double totalDuration = static_cast<double>(frameCount) / baseFps;
    return (currentTime - startTime) >= totalDuration;
}

void HitFXSystem::triggerAudio(const HitEvent&             ev,
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

    std::string sfxKey = (effectiveType == ::MMM::NoteType::FLICK)
                             ? "hiteffect.flick"
                             : "hiteffect.note";

    audioManager.playSoundEffectScheduled(sfxKey, ev.timestamp, volumeFactor);
}

void HitFXSystem::triggerVisual(const HitEvent&             ev,
                                const Config::EditorConfig& config)
{
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
    newEffect.startTime   = ev.timestamp;
    newEffect.duration    = ev.duration;
    newEffect.trackIndex  = ev.trackIndex;
    newEffect.trackSpan   = ev.trackSpan;
    newEffect.trackOffset = ev.trackOffset;
    newEffect.isLooping   = (ev.type == ::MMM::NoteType::HOLD);
    newEffect.effectKey   = effectKey;

    m_trackActiveEffects[ev.trackIndex] = newEffect;
}

void HitFXSystem::update(double visualTime, const std::vector<HitEvent>& events,
                         const Config::EditorConfig& config)
{
    for ( const auto& ev : events ) {
        triggerVisual(ev, config);
    }

    auto& skinManager = Config::SkinManager::instance();
    // 4. 清理已经播放完成的非循环特效
    float baseFps = skinManager.getEffectBaseFps();
    for ( auto it = m_trackActiveEffects.begin();
          it != m_trackActiveEffects.end(); ) {
        auto& active = it->second;

        const auto* seq =
            skinManager.getEffectSequence("note.effect." + active.effectKey);
        size_t frameCount = seq ? seq->frames.size() : 0;

        if ( active.isLooping ) {
            // 对于 Hold，如果当前时间超过了 Hold 结束时间，且至少播放完一个完整的普通动画周期，则结束
            // 这确保了极短或 0 时长的 Hold 也能正常播放完一个完整的打击动画
            double animDuration = static_cast<double>(frameCount) / baseFps;
            if ( visualTime > (active.startTime + active.duration) &&
                 visualTime >= (active.startTime + animDuration) ) {
                it = m_trackActiveEffects.erase(it);
                continue;
            }
        } else {
            // 普通物件播放一次即结束
            if ( active.isFinished(visualTime, baseFps, frameCount) ) {
                it = m_trackActiveEffects.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void HitFXSystem::generateSnapshot(Batcher& batcher, double visualTime,
                                   const Config::EditorConfig& config,
                                   int32_t trackCount, float judgmentLineY,
                                   float leftX, float singleTrackW)
{
    if ( m_trackActiveEffects.empty() ) return;

    RenderSnapshot* snapshot    = batcher.snapshot;
    auto&           skinManager = Config::SkinManager::instance();
    float           baseFps     = skinManager.getEffectBaseFps();

    for ( const auto& [track, active] : m_trackActiveEffects ) {
        const auto* seq =
            skinManager.getEffectSequence("note.effect." + active.effectKey);
        if ( !seq || seq->frames.empty() ) continue;

        size_t frameCount = seq->frames.size();
        double elapsed    = visualTime - active.startTime;
        if ( elapsed < 0 )
            continue;  // 尚未到达触发点（虽然 logic 逻辑应该保证触发）

        size_t frameIndex = 0;
        if ( active.isLooping ) {
            // Hold 循环播放动画
            frameIndex =
                static_cast<size_t>(std::floor(elapsed * baseFps)) % frameCount;
        } else {
            // 普通/Flick 播放一次
            frameIndex = static_cast<size_t>(std::floor(elapsed * baseFps));
            if ( frameIndex >= frameCount ) continue;  // 已播放完（防御性判断）
        }

        // 使用 SkinManager 统一分配好的起始 ID
        uint32_t textureId = seq->startId + static_cast<uint32_t>(frameIndex);

        // 获取特效序列帧的 UV 信息以计算比例
        auto itTex = snapshot->uvMap.find(textureId);
        if ( itTex == snapshot->uvMap.end() ) continue;
        float texAspect = itTex->second.z / itTex->second.w;

        // 计算基准尺寸：特效宽度 = 音符宽度 * 2.5 (稍微大一点)
        float noteW   = singleTrackW * config.visual.noteScaleX;
        float effectW = noteW;
        float effectH = effectW / texAspect;

        // 计算位置：
        // 打击点中心 X：起始轨道 X + (偏移量 + 0.5) * 轨道宽
        // 对于 Flick，trackOffset 即为 dtrack
        float centerX =
            leftX +
            (active.trackIndex + active.trackOffset + 0.5f) * singleTrackW;
        float centerY = judgmentLineY;

        float x = centerX - effectW * 0.5f;
        float y = centerY + effectH * 0.5f;

        batcher.setTexture(static_cast<TextureID>(textureId));
        // 使用 pushFilledQuad 以保证采样一致性并适配 FillMode
        batcher.pushFilledQuad(x,
                               y,
                               effectW,
                               effectH,
                               { texAspect, 1.0f },
                               config.visual.noteFillMode,
                               glm::vec4(1.0f));
    }

    batcher.flush();
}

}  // namespace MMM::Logic::System
