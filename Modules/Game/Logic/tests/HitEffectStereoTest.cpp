#include "logic/ecs/system/HitFXSystem.h"

#include "audio/StereoGainEnvelope.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace
{

/// @brief 使用小容差比较声道增益。
/// @param lhs 左值。
/// @param rhs 右值。
/// @return 两个数值足够接近时返回 true。
bool near(float lhs, float rhs)
{
    return std::abs(lhs - rhs) < 1e-6F;
}

/// @brief 创建用于声像计算测试的打击事件。
/// @param type 物件类型。
/// @param trackIndex 零起始轨道索引。
/// @param trackOffset Flick 滑动轨道偏移。
/// @return 固定时间的打击事件。
MMM::Logic::System::HitFXSystem::HitEvent makeEvent(MMM::NoteType type,
                                                    int           trackIndex,
                                                    int trackOffset = 0)
{
    using HitEvent = MMM::Logic::System::HitFXSystem::HitEvent;
    return {
        0.0, type, HitEvent::Role::None, 1, trackIndex, trackOffset, 0.0, false,
    };
}

/// @brief 验证普通物件按物件中心获得固定双声道音量。
/// @return 四轨第二轨物件得到左 0.625、右 0.375 时返回 true。
bool testStaticTrackPosition()
{
    const auto envelope =
        MMM::Logic::System::HitFXSystem::stereoGainEnvelopeForEvent(
            makeEvent(MMM::NoteType::NOTE, 1), 4, true);
    if ( !near(envelope.startLeft, 0.625F) ||
         !near(envelope.startRight, 0.375F) ||
         !near(envelope.endLeft, 0.625F) || !near(envelope.endRight, 0.375F) ||
         !near(envelope.startLeft + envelope.startRight, 1.0F) ) {
        XERROR("Static hit effect stereo position did not match track center");
        return false;
    }
    return true;
}

/// @brief 验证 Flick 音效从起始轨道线性移动到目标轨道。
/// @return 四轨第二轨滑向第三轨时首尾和中点音量符合预期。
bool testFlickMovesAcrossChannels()
{
    const auto envelope =
        MMM::Logic::System::HitFXSystem::stereoGainEnvelopeForEvent(
            makeEvent(MMM::NoteType::FLICK, 1, 1), 4, true);
    const auto middle = MMM::Audio::stereoGainAtProgress(envelope, 0.5F);
    if ( !near(envelope.startLeft, 0.625F) ||
         !near(envelope.startRight, 0.375F) ||
         !near(envelope.endLeft, 0.375F) || !near(envelope.endRight, 0.625F) ||
         !near(middle.left, 0.5F) || !near(middle.right, 0.5F) ||
         !near(envelope.endLeft + envelope.endRight, 1.0F) ) {
        XERROR("Flick hit effect did not move linearly between track centers");
        return false;
    }
    return true;
}

/// @brief 验证负坐标草稿轨正确映射到草稿区局部轨道和声像。
/// @return 四轨草稿最左与最右轨分别映射到局部 0 和 3 时返回 true。
bool testDraftTrackMappingAndStereo()
{
    using HitFXSystem  = MMM::Logic::System::HitFXSystem;
    auto leftDraft     = makeEvent(MMM::NoteType::NOTE, -4);
    auto rightDraft    = makeEvent(MMM::NoteType::NOTE, -1);
    leftDraft.isDraft  = true;
    rightDraft.isDraft = true;

    const auto leftEnvelope =
        HitFXSystem::stereoGainEnvelopeForEvent(leftDraft, 4, true);
    const auto rightEnvelope =
        HitFXSystem::stereoGainEnvelopeForEvent(rightDraft, 4, true);
    if ( HitFXSystem::areaTrackIndexForEvent(leftDraft, 4) != 0 ||
         HitFXSystem::areaTrackIndexForEvent(rightDraft, 4) != 3 ||
         !near(leftEnvelope.startLeft, 0.875F) ||
         !near(leftEnvelope.startRight, 0.125F) ||
         !near(rightEnvelope.startLeft, 0.125F) ||
         !near(rightEnvelope.startRight, 0.875F) ) {
        XERROR("Draft hit event did not use draft-local lane mapping");
        return false;
    }
    return true;
}

/// @brief 验证画面两侧轨道与实际左右声道方向一致。
/// @return 最左轨左声道更响且最右轨右声道更响时返回 true。
bool testTrackSidesMatchChannels()
{
    const auto leftTrack =
        MMM::Logic::System::HitFXSystem::stereoGainEnvelopeForEvent(
            makeEvent(MMM::NoteType::NOTE, 0), 4, true);
    const auto rightTrack =
        MMM::Logic::System::HitFXSystem::stereoGainEnvelopeForEvent(
            makeEvent(MMM::NoteType::NOTE, 3), 4, true);
    if ( leftTrack.startLeft <= leftTrack.startRight ||
         rightTrack.startRight <= rightTrack.startLeft ) {
        XERROR("Hit effect stereo channels were mirrored across the canvas");
        return false;
    }
    return true;
}

/// @brief 验证关闭功能时保留未经衰减的原始立体声音效。
/// @return 首尾左右声道增益均为 1 时返回 true。
bool testDisabledKeepsOriginalStereo()
{
    const auto envelope =
        MMM::Logic::System::HitFXSystem::stereoGainEnvelopeForEvent(
            makeEvent(MMM::NoteType::FLICK, 1, 1), 4, false);
    if ( !near(envelope.startLeft, 1.0F) || !near(envelope.startRight, 1.0F) ||
         !near(envelope.endLeft, 1.0F) || !near(envelope.endRight, 1.0F) ) {
        XERROR("Disabled stereo hit effects changed the original channel gain");
        return false;
    }
    return true;
}

/// @brief 验证绑定音效严格优先于内置 Note/Flick 音效。
/// @return 非空绑定返回原资源，空绑定按物件类型返回内置资源时返回 true。
bool testBoundSoundOverridesDefault()
{
    using HitFXSystem = MMM::Logic::System::HitFXSystem;

    auto boundEvent          = makeEvent(MMM::NoteType::FLICK, 1, 1);
    boundEvent.sampleBinding = MMM::AudioSampleBinding{ "sample.wav", 0.35F };
    if ( HitFXSystem::soundEffectKeyForEvent(
             boundEvent, MMM::NoteType::FLICK) != "sample.wav" ) {
        XERROR("Bound note sound did not override the built-in Flick sound");
        return false;
    }

    const auto noteEvent  = makeEvent(MMM::NoteType::NOTE, 0);
    const auto flickEvent = makeEvent(MMM::NoteType::FLICK, 0, 1);
    if ( HitFXSystem::soundEffectKeyForEvent(noteEvent, MMM::NoteType::NOTE) !=
             "hiteffect.note" ||
         HitFXSystem::soundEffectKeyForEvent(
             flickEvent, MMM::NoteType::FLICK) != "hiteffect.flick" ) {
        XERROR("Empty bound sound did not select the built-in hit effect");
        return false;
    }
    return true;
}

/// @brief 验证自定义采样的物件音量独立进入打击音效倍率。
/// @return 有绑定时返回物件音量、无绑定时返回 1。
bool testBoundSampleVolume()
{
    using HitFXSystem = MMM::Logic::System::HitFXSystem;

    auto boundEvent          = makeEvent(MMM::NoteType::NOTE, 1);
    boundEvent.sampleBinding = MMM::AudioSampleBinding{ "sample.wav", 0.35F };
    if ( !near(HitFXSystem::sampleVolumeForEvent(boundEvent), 0.35F) ||
         !near(HitFXSystem::sampleVolumeForEvent(
                   makeEvent(MMM::NoteType::NOTE, 1)),
               1.0F) ) {
        XERROR("Bound sample volume was not applied independently");
        return false;
    }
    return true;
}

/// @brief 验证仅非空资源绑定会进入已绑定打击音效分组。
/// @return 无绑定和空绑定为未绑定，非空绑定为已绑定时返回 true。
bool testBoundSoundClassification()
{
    using HitFXSystem = MMM::Logic::System::HitFXSystem;

    auto emptyBindingEvent          = makeEvent(MMM::NoteType::NOTE, 0);
    emptyBindingEvent.sampleBinding = MMM::AudioSampleBinding{ "", 0.5F };
    auto boundEvent                 = makeEvent(MMM::NoteType::NOTE, 0);
    boundEvent.sampleBinding = MMM::AudioSampleBinding{ "sample.wav", 0.5F };
    if ( HitFXSystem::hasBoundSoundEffect(makeEvent(MMM::NoteType::NOTE, 0)) ||
         HitFXSystem::hasBoundSoundEffect(emptyBindingEvent) ||
         !HitFXSystem::hasBoundSoundEffect(boundEvent) ) {
        XERROR("Hit sound binding groups were classified incorrectly");
        return false;
    }
    return true;
}

/// @brief 验证旧皮肤的固定模式仍在判定线中心按原尺寸绘制。
/// @return 固定矩形的中心、宽高与旧算法一致时返回 true。
bool testFixedHitEffectBounds()
{
    using HitFXSystem = MMM::Logic::System::HitFXSystem;
    const auto bounds = HitFXSystem::calculateRenderBounds(
        MMM::Config::HitEffectLayoutMode::Fixed,
        4,
        1,
        1,
        300.0F,
        100.0F,
        20.0F,
        500.0F,
        50.0F,
        40.0F,
        20.0F);
    if ( !near(bounds.x, 205.0F) || !near(bounds.y, 310.0F) ||
         !near(bounds.width, 40.0F) || !near(bounds.height, 20.0F) ) {
        XERROR("Fixed hit effect bounds no longer match legacy placement");
        return false;
    }
    return true;
}

/// @brief 验证草稿事件不会进入玩家轨道 KPS 统计。
/// @return 草稿 Flick 即使偏移落入非负轨道也不增加玩家 KPS 时返回 true。
bool testDraftEventsDoNotAffectPlayerKps()
{
    using HitFXSystem  = MMM::Logic::System::HitFXSystem;
    auto draftFlick    = makeEvent(MMM::NoteType::FLICK, -1, 1);
    draftFlick.isDraft = true;

    MMM::Config::EditorConfig config;
    config.visual.canvasComponents.kps.visible = true;
    HitFXSystem system;
    system.update(0.0, { draftFlick }, 4, config);
    const auto kps = system.trackKps();
    if ( kps.size() != 4U ||
         std::any_of(kps.begin(), kps.end(), [](std::uint32_t count) {
             return count != 0U;
         }) ) {
        XERROR("Draft hit event leaked into player KPS statistics");
        return false;
    }
    return true;
}

/// @brief 验证整轨模式覆盖 Flick 目标轨道的完整可见区域。
/// @return 目标轨道宽度和上下边界均精确匹配时返回 true。
bool testTrackFillHitEffectBounds()
{
    using HitFXSystem = MMM::Logic::System::HitFXSystem;
    const auto bounds = HitFXSystem::calculateRenderBounds(
        MMM::Config::HitEffectLayoutMode::TrackFill,
        4,
        1,
        1,
        300.0F,
        100.0F,
        20.0F,
        500.0F,
        50.0F,
        40.0F,
        20.0F);
    if ( !near(bounds.x, 200.0F) || !near(bounds.y, 500.0F) ||
         !near(bounds.width, 50.0F) || !near(bounds.height, 480.0F) ) {
        XERROR("Track-fill hit effect did not cover the destination track");
        return false;
    }
    return true;
}

/// @brief 验证非 Hold 特效按视觉时长结束并循环序列帧。
/// @return 时长边界正确且超过一轮后回到对应帧时返回 true。
bool testNonHoldHitEffectPlayback()
{
    using HitFXSystem = MMM::Logic::System::HitFXSystem;
    if ( HitFXSystem::isNonHoldEffectFinished(0.119, 0.12F) ||
         !HitFXSystem::isNonHoldEffectFinished(0.12, 0.12F) ) {
        XERROR("Non-Hold hit effect duration boundary was not respected");
        return false;
    }

    const auto firstLoopFrame =
        HitFXSystem::loopingEffectFrameIndex(0.125, 60.0F, 6U);
    const auto invalidFrame =
        HitFXSystem::loopingEffectFrameIndex(0.125, 0.0F, 6U);
    if ( !firstLoopFrame || *firstLoopFrame != 1U || invalidFrame ) {
        XERROR("Non-Hold hit effect frames did not loop safely");
        return false;
    }
    return true;
}

/// @brief 验证从持续区间中段播放时补建普通 Hold 与 Polyline subHold 特效。
/// @return 两类有效 Hold 均补建且已结束或未开始的事件被忽略时返回 true。
bool testRestoreActiveHoldEffectsFromMiddle()
{
    using HitFXSystem = MMM::Logic::System::HitFXSystem;
    using HitEvent    = HitFXSystem::HitEvent;

    const std::vector<HitEvent> events{
        { 0.5,
          MMM::NoteType::HOLD,
          HitEvent::Role::None,
          1,
          3,
          0,
          0.25,
          false },
        { 1.0, MMM::NoteType::HOLD, HitEvent::Role::None, 1, 0, 0, 4.0, false },
        { 2.0,
          MMM::NoteType::HOLD,
          HitEvent::Role::Internal,
          1,
          1,
          0,
          2.0,
          true },
        { 2.5, MMM::NoteType::NOTE, HitEvent::Role::None, 1, 2, 0, 0.0, false },
        { 4.0, MMM::NoteType::HOLD, HitEvent::Role::None, 1, 2, 0, 1.0, false },
    };
    MMM::Config::EditorConfig config;
    HitFXSystem               system;
    const std::size_t         restoredCount =
        system.restoreActiveHoldEffects(3.0, events, config);
    if ( restoredCount != 2U ) {
        XERROR("Playback middle did not restore Hold and subHold effects");
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行 HitEffect 立体声定位测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testStaticTrackPosition() && testFlickMovesAcrossChannels() &&
                   testDraftTrackMappingAndStereo() &&
                   testTrackSidesMatchChannels() &&
                   testDisabledKeepsOriginalStereo() &&
                   testBoundSoundOverridesDefault() &&
                   testBoundSampleVolume() && testBoundSoundClassification() &&
                   testFixedHitEffectBounds() &&
                   testDraftEventsDoNotAffectPlayerKps() &&
                   testTrackFillHitEffectBounds() &&
                   testNonHoldHitEffectPlayback() &&
                   testRestoreActiveHoldEffectsFromMiddle()
               ? 0
               : 1;
}
