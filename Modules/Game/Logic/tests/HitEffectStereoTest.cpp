#include "logic/ecs/system/HitFXSystem.h"

#include "audio/StereoGainEnvelope.h"
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
/// @return 四轨第二轨物件得到左 0.375、右 0.625 时返回 true。
bool testStaticTrackPosition()
{
    const auto envelope =
        MMM::Logic::System::HitFXSystem::stereoGainEnvelopeForEvent(
            makeEvent(MMM::NoteType::NOTE, 1), 4, true);
    if ( !near(envelope.startLeft, 0.375F) ||
         !near(envelope.startRight, 0.625F) ||
         !near(envelope.endLeft, 0.375F) || !near(envelope.endRight, 0.625F) ||
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
    if ( !near(envelope.startLeft, 0.375F) ||
         !near(envelope.startRight, 0.625F) ||
         !near(envelope.endLeft, 0.625F) || !near(envelope.endRight, 0.375F) ||
         !near(middle.left, 0.5F) || !near(middle.right, 0.5F) ||
         !near(envelope.endLeft + envelope.endRight, 1.0F) ) {
        XERROR("Flick hit effect did not move linearly between track centers");
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

}  // namespace

/// @brief 运行 HitEffect 立体声定位测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testStaticTrackPosition() && testFlickMovesAcrossChannels() &&
                   testDisabledKeepsOriginalStereo()
               ? 0
               : 1;
}
