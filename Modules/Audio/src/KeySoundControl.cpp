#include "audio/KeySoundControl.h"

#include <algorithm>
#include <cmath>

namespace MMM::Audio
{
namespace
{
/// @brief Key 音运行时增益允许的最大线性倍率。
constexpr float MAX_KEY_SOUND_GAIN = 2.0F;

/// @brief 16 位定点增益的最大整数值。
constexpr std::uint32_t MAX_QUANTIZED_GAIN = 65535U;

/// @brief 使 1.0 可以精确表示的定点缩放倍率。
constexpr float GAIN_QUANTIZATION_SCALE = 32768.0F;
}  // namespace

std::uint32_t KeySoundControlBank::quantizeGain(float gain) noexcept
{
    const float sanitized =
        std::isfinite(gain) ? std::clamp(gain, 0.0F, MAX_KEY_SOUND_GAIN) : 0.0F;
    return std::min(static_cast<std::uint32_t>(
                        std::lround(sanitized * GAIN_QUANTIZATION_SCALE)),
                    MAX_QUANTIZED_GAIN);
}

std::uint32_t KeySoundControlBank::pack(
    const KeySoundControlSnapshot& control) noexcept
{
    return quantizeGain(control.gain) | (control.muted ? MUTED_FLAG : 0U);
}

KeySoundControlSnapshot KeySoundControlBank::unpack(
    std::uint32_t packed) noexcept
{
    const auto quantizedGain = packed & GAIN_QUANTIZED_MASK;
    return {
        .muted = (packed & MUTED_FLAG) != 0U,
        .gain  = static_cast<float>(quantizedGain) / GAIN_QUANTIZATION_SCALE,
    };
}

void KeySoundControlBank::setMuted(AtomicControl& control, bool muted) noexcept
{
    auto current = control.packed.load(std::memory_order_relaxed);
    while ( true ) {
        const auto next = muted ? current | MUTED_FLAG : current & ~MUTED_FLAG;
        if ( control.packed.compare_exchange_weak(current,
                                                  next,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed) ) {
            return;
        }
    }
}

void KeySoundControlBank::setGain(AtomicControl& control, float gain) noexcept
{
    const auto quantizedGain = quantizeGain(gain);
    auto       current       = control.packed.load(std::memory_order_relaxed);
    while ( true ) {
        const auto next = (current & ~GAIN_QUANTIZED_MASK) | quantizedGain;
        if ( control.packed.compare_exchange_weak(current,
                                                  next,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed) ) {
            return;
        }
    }
}

KeySoundControlSnapshot KeySoundControlBank::load(
    const AtomicControl& control) noexcept
{
    return unpack(control.packed.load(std::memory_order_relaxed));
}

std::size_t KeySoundControlBank::groupIndex(KeySoundEffectGroup group) noexcept
{
    return group == KeySoundEffectGroup::Bound ? 1U : 0U;
}

void KeySoundControlBank::setPlayerAreaMuted(bool muted) noexcept
{
    setMuted(m_playerArea, muted);
}

bool KeySoundControlBank::isPlayerAreaMuted() const noexcept
{
    return load(m_playerArea).muted;
}

void KeySoundControlBank::setPlayerTrackMuted(std::uint32_t trackIndex,
                                              bool          muted) noexcept
{
    if ( trackIndex >= KEY_SOUND_TRACK_LIMIT ) return;
    setMuted(m_playerTracks[trackIndex], muted);
}

bool KeySoundControlBank::isPlayerTrackMuted(
    std::uint32_t trackIndex) const noexcept
{
    return trackIndex < KEY_SOUND_TRACK_LIMIT
               ? load(m_playerTracks[trackIndex]).muted
               : false;
}

void KeySoundControlBank::setPlayerTrackGain(std::uint32_t trackIndex,
                                             float         gain) noexcept
{
    if ( trackIndex >= KEY_SOUND_TRACK_LIMIT ) return;
    setGain(m_playerTracks[trackIndex], gain);
}

void KeySoundControlBank::setPlayerTrackControl(
    std::uint32_t trackIndex, const KeySoundControlSnapshot& control) noexcept
{
    if ( trackIndex >= KEY_SOUND_TRACK_LIMIT ) return;
    m_playerTracks[trackIndex].packed.store(pack(control),
                                            std::memory_order_relaxed);
}

float KeySoundControlBank::getPlayerTrackGain(
    std::uint32_t trackIndex) const noexcept
{
    return trackIndex < KEY_SOUND_TRACK_LIMIT
               ? load(m_playerTracks[trackIndex]).gain
               : 1.0F;
}

KeySoundControlSnapshot KeySoundControlBank::getPlayerTrackControl(
    std::uint32_t trackIndex) const noexcept
{
    return trackIndex < KEY_SOUND_TRACK_LIMIT ? load(m_playerTracks[trackIndex])
                                              : KeySoundControlSnapshot{};
}

void KeySoundControlBank::setDraftAreaMuted(bool muted) noexcept
{
    setMuted(m_draftArea, muted);
}

bool KeySoundControlBank::isDraftAreaMuted() const noexcept
{
    return load(m_draftArea).muted;
}

void KeySoundControlBank::setDraftTrackMuted(std::uint32_t trackIndex,
                                             bool          muted) noexcept
{
    if ( trackIndex >= KEY_SOUND_TRACK_LIMIT ) return;
    setMuted(m_draftTracks[trackIndex], muted);
}

bool KeySoundControlBank::isDraftTrackMuted(
    std::uint32_t trackIndex) const noexcept
{
    return trackIndex < KEY_SOUND_TRACK_LIMIT
               ? load(m_draftTracks[trackIndex]).muted
               : false;
}

void KeySoundControlBank::setDraftTrackGain(std::uint32_t trackIndex,
                                            float         gain) noexcept
{
    if ( trackIndex >= KEY_SOUND_TRACK_LIMIT ) return;
    setGain(m_draftTracks[trackIndex], gain);
}

float KeySoundControlBank::getDraftTrackGain(
    std::uint32_t trackIndex) const noexcept
{
    return trackIndex < KEY_SOUND_TRACK_LIMIT
               ? load(m_draftTracks[trackIndex]).gain
               : 1.0F;
}

void KeySoundControlBank::setBgmAreaMuted(bool muted) noexcept
{
    setMuted(m_bgmArea, muted);
}

bool KeySoundControlBank::isBgmAreaMuted() const noexcept
{
    return load(m_bgmArea).muted;
}

void KeySoundControlBank::setBgmAreaGain(float gain) noexcept
{
    setGain(m_bgmArea, gain);
}

float KeySoundControlBank::getBgmAreaGain() const noexcept
{
    return load(m_bgmArea).gain;
}

void KeySoundControlBank::setBgmTrackMuted(std::uint32_t trackIndex,
                                           bool          muted) noexcept
{
    if ( trackIndex >= KEY_SOUND_TRACK_LIMIT ) return;
    setMuted(m_bgmTracks[trackIndex], muted);
}

bool KeySoundControlBank::isBgmTrackMuted(
    std::uint32_t trackIndex) const noexcept
{
    return trackIndex < KEY_SOUND_TRACK_LIMIT
               ? load(m_bgmTracks[trackIndex]).muted
               : false;
}

void KeySoundControlBank::setBgmTrackGain(std::uint32_t trackIndex,
                                          float         gain) noexcept
{
    if ( trackIndex >= KEY_SOUND_TRACK_LIMIT ) return;
    setGain(m_bgmTracks[trackIndex], gain);
}

float KeySoundControlBank::getBgmTrackGain(
    std::uint32_t trackIndex) const noexcept
{
    return trackIndex < KEY_SOUND_TRACK_LIMIT
               ? load(m_bgmTracks[trackIndex]).gain
               : 1.0F;
}

void KeySoundControlBank::setEffectGroupMuted(KeySoundEffectGroup group,
                                              bool muted) noexcept
{
    setMuted(m_effectGroups[groupIndex(group)], muted);
}

bool KeySoundControlBank::isEffectGroupMuted(
    KeySoundEffectGroup group) const noexcept
{
    return load(m_effectGroups[groupIndex(group)]).muted;
}

void KeySoundControlBank::setEffectGroupGain(KeySoundEffectGroup group,
                                             float               gain) noexcept
{
    setGain(m_effectGroups[groupIndex(group)], gain);
}

float KeySoundControlBank::getEffectGroupGain(
    KeySoundEffectGroup group) const noexcept
{
    return load(m_effectGroups[groupIndex(group)]).gain;
}

float KeySoundControlBank::effectivePlayerGain(
    const KeySoundPlaybackControl& control) const noexcept
{
    if ( !control.enabled ) return 1.0F;

    const bool isDraft = control.area == KeySoundPlaybackArea::Draft;
    const auto area    = isDraft ? load(m_draftArea) : load(m_playerArea);
    if ( area.muted ) return 0.0F;

    KeySoundControlSnapshot track;
    if ( control.playerTrackIndex < KEY_SOUND_TRACK_LIMIT ) {
        track = isDraft ? load(m_draftTracks[control.playerTrackIndex])
                        : load(m_playerTracks[control.playerTrackIndex]);
        if ( track.muted ) return 0.0F;
    }

    const auto group = load(m_effectGroups[groupIndex(control.effectGroup)]);
    if ( group.muted ) return 0.0F;
    return area.gain * track.gain * group.gain;
}

float KeySoundControlBank::effectiveBgmTrackGain(
    std::uint32_t trackIndex) const noexcept
{
    const auto area = load(m_bgmArea);
    if ( area.muted ) return 0.0F;

    if ( trackIndex >= KEY_SOUND_TRACK_LIMIT ) return area.gain;
    const auto track = load(m_bgmTracks[trackIndex]);
    return track.muted ? 0.0F : area.gain * track.gain;
}

}  // namespace MMM::Audio
