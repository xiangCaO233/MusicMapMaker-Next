#include "audio/KeySoundControl.h"

#include "log/colorful-log.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>

namespace
{

constexpr float CONTROL_EPSILON = 4.0e-5F;

/// @brief 判断量化后的 Key 音增益是否符合预期。
bool nearlyEqual(float actual, float expected) noexcept
{
    return std::abs(actual - expected) <= CONTROL_EPSILON;
}

/// @brief 验证所有控制项默认全开且使用单位增益。
bool testDefaults()
{
    MMM::Audio::KeySoundControlBank controls;
    const auto player = MMM::Audio::KeySoundPlaybackControl{
        .enabled          = true,
        .playerTrackIndex = 12U,
        .effectGroup      = MMM::Audio::KeySoundEffectGroup::Unbound,
    };

    const bool valid =
        !controls.isPlayerAreaMuted() && !controls.isPlayerTrackMuted(12U) &&
        nearlyEqual(controls.getPlayerTrackGain(12U), 1.0F) &&
        !controls.isBgmAreaMuted() &&
        nearlyEqual(controls.getBgmAreaGain(), 1.0F) &&
        !controls.isBgmTrackMuted(7U) &&
        nearlyEqual(controls.getBgmTrackGain(7U), 1.0F) &&
        !controls.isEffectGroupMuted(
            MMM::Audio::KeySoundEffectGroup::Unbound) &&
        !controls.isEffectGroupMuted(MMM::Audio::KeySoundEffectGroup::Bound) &&
        nearlyEqual(controls.getEffectGroupGain(
                        MMM::Audio::KeySoundEffectGroup::Unbound),
                    1.0F) &&
        nearlyEqual(
            controls.getEffectGroupGain(MMM::Audio::KeySoundEffectGroup::Bound),
            1.0F) &&
        nearlyEqual(controls.effectivePlayerGain(player), 1.0F) &&
        nearlyEqual(controls.effectiveBgmTrackGain(7U), 1.0F);
    if ( !valid ) XERROR("Key sound control defaults are not unity");
    return valid;
}

/// @brief 验证静音修改保留增益，增益输入被限制到 0~2。
bool testMutePreservesGainAndClamping()
{
    MMM::Audio::KeySoundControlBank controls;
    controls.setPlayerTrackGain(3U, 1.25F);
    controls.setPlayerTrackMuted(3U, true);
    controls.setPlayerTrackMuted(3U, false);
    if ( !nearlyEqual(controls.getPlayerTrackGain(3U), 1.25F) ) {
        XERROR("Player track mute overwrote its gain");
        return false;
    }

    controls.setPlayerTrackGain(3U, -1.0F);
    if ( !nearlyEqual(controls.getPlayerTrackGain(3U), 0.0F) ) return false;
    controls.setPlayerTrackGain(3U, 9.0F);
    if ( !nearlyEqual(controls.getPlayerTrackGain(3U), 2.0F) ) return false;
    controls.setPlayerTrackGain(3U, std::numeric_limits<float>::quiet_NaN());
    if ( !nearlyEqual(controls.getPlayerTrackGain(3U), 0.0F) ) return false;

    controls.setBgmAreaGain(std::numeric_limits<float>::infinity());
    controls.setEffectGroupGain(MMM::Audio::KeySoundEffectGroup::Bound, -3.0F);
    const bool valid = nearlyEqual(controls.getBgmAreaGain(), 0.0F) &&
                       nearlyEqual(controls.getEffectGroupGain(
                                       MMM::Audio::KeySoundEffectGroup::Bound),
                                   0.0F);
    if ( !valid ) XERROR("Key sound gains were not clamped safely");
    return valid;
}

/// @brief 验证玩家轨道、类别和 BGM 区域使用正确的乘法组合。
bool testGainComposition()
{
    MMM::Audio::KeySoundControlBank controls;
    const auto playback = MMM::Audio::KeySoundPlaybackControl{
        .enabled          = true,
        .playerTrackIndex = 4U,
        .effectGroup      = MMM::Audio::KeySoundEffectGroup::Bound,
    };

    controls.setPlayerTrackGain(4U, 0.5F);
    controls.setEffectGroupGain(MMM::Audio::KeySoundEffectGroup::Bound, 1.5F);
    if ( !nearlyEqual(controls.effectivePlayerGain(playback), 0.75F) ) {
        XERROR("Player track and effect group gains were not composed");
        return false;
    }

    controls.setPlayerTrackMuted(4U, true);
    if ( controls.effectivePlayerGain(playback) != 0.0F ) return false;
    controls.setPlayerTrackMuted(4U, false);
    controls.setEffectGroupMuted(MMM::Audio::KeySoundEffectGroup::Bound, true);
    if ( controls.effectivePlayerGain(playback) != 0.0F ) return false;
    controls.setEffectGroupMuted(MMM::Audio::KeySoundEffectGroup::Bound, false);
    controls.setPlayerAreaMuted(true);
    if ( controls.effectivePlayerGain(playback) != 0.0F ) return false;

    const auto ordinarySfx = MMM::Audio::KeySoundPlaybackControl{};
    if ( !nearlyEqual(controls.effectivePlayerGain(ordinarySfx), 1.0F) ) {
        XERROR("Ordinary SFX unexpectedly inherited Key sound controls");
        return false;
    }

    controls.setBgmAreaGain(0.5F);
    controls.setBgmTrackGain(2U, 1.5F);
    if ( !nearlyEqual(controls.effectiveBgmTrackGain(2U), 0.75F) ) {
        XERROR("BGM area and track gains were not composed");
        return false;
    }
    controls.setBgmTrackMuted(2U, true);
    if ( controls.effectiveBgmTrackGain(2U) != 0.0F ) return false;
    controls.setBgmTrackMuted(2U, false);
    controls.setBgmAreaMuted(true);
    return controls.effectiveBgmTrackGain(2U) == 0.0F;
}

/// @brief 验证未绑定与已绑定打击音的静音和增益互不影响。
bool testEffectGroupIsolation()
{
    MMM::Audio::KeySoundControlBank controls;
    const auto unbound = MMM::Audio::KeySoundPlaybackControl{
        .enabled     = true,
        .effectGroup = MMM::Audio::KeySoundEffectGroup::Unbound,
    };
    const auto bound = MMM::Audio::KeySoundPlaybackControl{
        .enabled     = true,
        .effectGroup = MMM::Audio::KeySoundEffectGroup::Bound,
    };

    controls.setEffectGroupGain(MMM::Audio::KeySoundEffectGroup::Unbound,
                                0.25F);
    controls.setEffectGroupMuted(MMM::Audio::KeySoundEffectGroup::Unbound,
                                 true);
    if ( controls.effectivePlayerGain(unbound) != 0.0F ||
         !nearlyEqual(controls.effectivePlayerGain(bound), 1.0F) ||
         controls.isEffectGroupMuted(MMM::Audio::KeySoundEffectGroup::Bound) ) {
        XERROR("Unbound Key sound controls leaked into Bound controls");
        return false;
    }

    controls.setEffectGroupMuted(MMM::Audio::KeySoundEffectGroup::Unbound,
                                 false);
    controls.setEffectGroupGain(MMM::Audio::KeySoundEffectGroup::Bound, 1.5F);
    controls.setEffectGroupMuted(MMM::Audio::KeySoundEffectGroup::Bound, true);
    const bool valid =
        controls.effectivePlayerGain(bound) == 0.0F &&
        nearlyEqual(controls.effectivePlayerGain(unbound), 0.25F) &&
        !controls.isEffectGroupMuted(
            MMM::Audio::KeySoundEffectGroup::Unbound) &&
        nearlyEqual(controls.getEffectGroupGain(
                        MMM::Audio::KeySoundEffectGroup::Unbound),
                    0.25F);
    if ( !valid )
        XERROR("Bound Key sound controls leaked into Unbound controls");
    return valid;
}

/// @brief 验证越界轨道不会写入固定控制库且读取为单位值。
bool testOutOfRangeTracks()
{
    MMM::Audio::KeySoundControlBank controls;
    constexpr std::uint32_t         INVALID_TRACK =
        std::numeric_limits<std::uint32_t>::max();
    controls.setPlayerTrackMuted(INVALID_TRACK, true);
    controls.setPlayerTrackGain(INVALID_TRACK, 0.0F);
    controls.setBgmTrackMuted(INVALID_TRACK, true);
    controls.setBgmTrackGain(INVALID_TRACK, 0.0F);
    controls.setPlayerTrackControl(INVALID_TRACK,
                                   { .muted = true, .gain = 0.0F });

    const auto snapshot = controls.getPlayerTrackControl(INVALID_TRACK);
    const bool valid =
        !controls.isPlayerTrackMuted(INVALID_TRACK) &&
        nearlyEqual(controls.getPlayerTrackGain(INVALID_TRACK), 1.0F) &&
        !snapshot.muted && nearlyEqual(snapshot.gain, 1.0F) &&
        !controls.isBgmTrackMuted(INVALID_TRACK) &&
        nearlyEqual(controls.getBgmTrackGain(INVALID_TRACK), 1.0F) &&
        nearlyEqual(controls.effectiveBgmTrackGain(INVALID_TRACK), 1.0F);
    if ( !valid ) XERROR("Out-of-range Key sound track was not unity");
    return valid;
}

/// @brief 验证并发发布期间读者只能看到两个完整 mute/gain 快照。
bool testConcurrentCompleteSnapshots()
{
    MMM::Audio::KeySoundControlBank controls;
    constexpr std::uint32_t         TRACK_INDEX = 19U;
    const auto                      first = MMM::Audio::KeySoundControlSnapshot{
        .muted = false,
        .gain  = 0.25F,
    };
    const auto second = MMM::Audio::KeySoundControlSnapshot{
        .muted = true,
        .gain  = 1.75F,
    };
    controls.setPlayerTrackControl(TRACK_INDEX, first);

    std::atomic_bool writerFinished{ false };
    std::atomic_bool invalidSnapshot{ false };
    std::thread      writer([&controls, &writerFinished, first, second]() {
        for ( std::size_t iteration = 0U; iteration < 200000U; ++iteration ) {
            controls.setPlayerTrackControl(
                TRACK_INDEX, (iteration & 1U) == 0U ? second : first);
        }
        writerFinished.store(true, std::memory_order_release);
    });

    do {
        const auto snapshot = controls.getPlayerTrackControl(TRACK_INDEX);
        const bool isFirst =
            !snapshot.muted && nearlyEqual(snapshot.gain, first.gain);
        const bool isSecond =
            snapshot.muted && nearlyEqual(snapshot.gain, second.gain);
        if ( !isFirst && !isSecond ) {
            invalidSnapshot.store(true, std::memory_order_relaxed);
            break;
        }
    } while ( !writerFinished.load(std::memory_order_acquire) );
    writer.join();

    if ( invalidSnapshot.load(std::memory_order_relaxed) ) {
        XERROR("Concurrent reader observed a torn Key sound snapshot");
        return false;
    }

    std::thread muteWriter([&controls]() {
        for ( std::size_t iteration = 0U; iteration < 100000U; ++iteration ) {
            controls.setPlayerTrackMuted(TRACK_INDEX, (iteration & 1U) != 0U);
        }
    });
    std::thread gainWriter([&controls]() {
        for ( std::size_t iteration = 0U; iteration < 100000U; ++iteration ) {
            controls.setPlayerTrackGain(TRACK_INDEX,
                                        (iteration & 1U) != 0U ? 1.5F : 0.5F);
        }
    });
    muteWriter.join();
    gainWriter.join();

    const auto merged = controls.getPlayerTrackControl(TRACK_INDEX);
    const bool valid  = merged.muted && nearlyEqual(merged.gain, 1.5F);
    if ( !valid ) XERROR("Concurrent field updates lost a Key sound value");
    return valid;
}

}  // namespace

/// @brief 运行固定容量 Key 音运行时控制测试。
int main()
{
    const bool passed = testDefaults() && testMutePreservesGainAndClamping() &&
                        testGainComposition() && testEffectGroupIsolation() &&
                        testOutOfRangeTracks() &&
                        testConcurrentCompleteSnapshots();
    return passed ? 0 : 1;
}
