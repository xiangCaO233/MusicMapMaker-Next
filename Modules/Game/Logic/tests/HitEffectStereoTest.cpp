#include "logic/ecs/system/HitFXSystem.h"

#include "audio/StereoGainEnvelope.h"
#include "config/skin/SkinConfig.h"
#include "log/colorful-log.h"

#include <cmath>

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

    auto boundEvent       = makeEvent(MMM::NoteType::FLICK, 1, 1);
    boundEvent.boundSound = "sample.wav";
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

}  // namespace

/// @brief 运行 HitEffect 立体声定位测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testStaticTrackPosition() && testFlickMovesAcrossChannels() &&
                   testTrackSidesMatchChannels() &&
                   testDisabledKeepsOriginalStereo() &&
                   testBoundSoundOverridesDefault() &&
                   testFixedHitEffectBounds() && testTrackFillHitEffectBounds()
               ? 0
               : 1;
}
