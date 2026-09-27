#include "audio/KeySoundControl.h"

#include <algorithm>
#include <cmath>

namespace MMM::Audio
{
namespace
{
// KeySound 控制用一个 32 位原子字同时保存 mute 与 gain：
//
// - 低 16 位是 0.0 到约 2.0 的定点线性增益；
// - 第 16 位是独立静音标志，其余高位保留；
// - 完整快照可由一次原子 load/store 发布，不会观察到字段撕裂；
// - 单字段更新使用 CAS，只替换自己的位并保留并发写入的另一字段；
// - relaxed 顺序足够，因为控制值彼此独立，不发布额外对象生命周期；
// - 所有数组容量固定，实时读取不进行哈希查询、锁或分配；
// - 越界轨道按单位控制读取，写入则忽略，避免异常索引影响其他轨道；
// - 最终有效增益按区域、轨道和类别相乘，任一级静音立即返回零。
// - 玩家与草稿共享音效类别，但区域总控和逐轨控制分别存储；
// - BGM 只组合区域与逐轨两级，不读取打击音效类别；
// - 默认 packed 值精确编码 1.0，构造时无需逐槽执行浮点量化；
// - 公开 getter 始终返回解码后的值，不暴露内部定点表示。

/// @brief Key 音运行时增益允许的最大线性倍率。
constexpr float MAX_KEY_SOUND_GAIN = 2.0F;

/// @brief 16 位定点增益的最大整数值。
constexpr std::uint32_t MAX_QUANTIZED_GAIN = 65535U;

/// @brief 使 1.0 可以精确表示的定点缩放倍率。
constexpr float GAIN_QUANTIZATION_SCALE = 32768.0F;
}  // namespace

/// @brief 规范化并编码一个线性增益。
/// @param gain 外部浮点增益。
/// @return 仅占低 16 位的定点值。
std::uint32_t KeySoundControlBank::quantizeGain(float gain) noexcept
{
    // 非有限值按静音增益处理，再限制到定点格式可表达的公开范围。
    const float sanitized =
        std::isfinite(gain) ? std::clamp(gain, 0.0F, MAX_KEY_SOUND_GAIN) : 0.0F;
    // lround 使正负量化误差对称；最终 min 防止 2.0 四舍五入越过 16 位。
    return std::min(static_cast<std::uint32_t>(
                        std::lround(sanitized * GAIN_QUANTIZATION_SCALE)),
                    MAX_QUANTIZED_GAIN);
}

/// @brief 把完整控制快照编码到一个原子字。
/// @param control 要同时发布的静音和增益。
/// @return 可直接原子存储的位布局。
std::uint32_t KeySoundControlBank::pack(
    const KeySoundControlSnapshot& control) noexcept
{
    // mute 占独立位，编码完整快照时不会丢失量化增益。
    return quantizeGain(control.gain) | (control.muted ? MUTED_FLAG : 0U);
}

/// @brief 从原子位布局恢复控制快照。
/// @param packed 单次原子读取取得的控制字。
/// @return 同一发布点的静音与增益。
KeySoundControlSnapshot KeySoundControlBank::unpack(
    std::uint32_t packed) noexcept
{
    // 先用掩码移除静音与保留位，再恢复为线性浮点增益。
    const auto quantizedGain = packed & GAIN_QUANTIZED_MASK;
    return {
        .muted = (packed & MUTED_FLAG) != 0U,
        .gain  = static_cast<float>(quantizedGain) / GAIN_QUANTIZATION_SCALE,
    };
}

/// @brief 原子更新一个控制项的静音位。
/// @param control 目标控制槽。
/// @param muted 新静音值。
void KeySoundControlBank::setMuted(AtomicControl& control, bool muted) noexcept
{
    // CAS 失败会把 current 更新为最新值，下一轮继续保留并发增益写入。
    auto current = control.packed.load(std::memory_order_relaxed);
    while ( true ) {
        // 只修改 MUTED_FLAG，低 16 位增益保持 current 中观察到的最新值。
        const auto next = muted ? current | MUTED_FLAG : current & ~MUTED_FLAG;
        if ( control.packed.compare_exchange_weak(current,
                                                  next,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed) ) {
            return;
        }
    }
}

/// @brief 原子更新一个控制项的增益位。
/// @param control 目标控制槽。
/// @param gain 将被规范化的线性增益。
void KeySoundControlBank::setGain(AtomicControl& control, float gain) noexcept
{
    // 量化在 CAS 循环外完成，竞争重试不会重复浮点换算。
    const auto quantizedGain = quantizeGain(gain);
    auto       current       = control.packed.load(std::memory_order_relaxed);
    while ( true ) {
        // 清除旧增益位后写入新值，静音位和未来保留位保持不变。
        const auto next = (current & ~GAIN_QUANTIZED_MASK) | quantizedGain;
        if ( control.packed.compare_exchange_weak(current,
                                                  next,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed) ) {
            return;
        }
    }
}

/// @brief 原子读取并解码一个控制槽。
/// @param control 只读目标槽。
/// @return 完整且不撕裂的控制快照。
KeySoundControlSnapshot KeySoundControlBank::load(
    const AtomicControl& control) noexcept
{
    // 单次 32 位原子读取即得到同一发布点的 mute/gain 快照。
    return unpack(control.packed.load(std::memory_order_relaxed));
}

/// @brief 把音效类别映射到固定数组索引。
/// @param group Bound 或 Unbound 类别。
/// @return 可安全访问两槽数组的索引。
std::size_t KeySoundControlBank::groupIndex(KeySoundEffectGroup group) noexcept
{
    // 固定两槽布局：Bound 使用一号，其余值安全回退到 Unbound 零号。
    return group == KeySoundEffectGroup::Bound ? 1U : 0U;
}

/// @brief 更新正式玩家区域的总静音覆盖。
/// @param muted 新静音状态。
void KeySoundControlBank::setPlayerAreaMuted(bool muted) noexcept
{
    setMuted(m_playerArea, muted);
}

/// @brief 查询正式玩家区域总静音。
/// @return 当前完整快照中的静音位。
bool KeySoundControlBank::isPlayerAreaMuted() const noexcept
{
    return load(m_playerArea).muted;
}

/// @brief 更新正式玩家区域的逐轨静音。
/// @param trackIndex 固定控制数组索引。
/// @param muted 新静音状态。
void KeySoundControlBank::setPlayerTrackMuted(std::uint32_t trackIndex,
                                              bool          muted) noexcept
{
    // 越界写入无副作用，固定数组不为异常谱面索引扩容。
    if ( trackIndex >= KEY_SOUND_TRACK_LIMIT ) return;
    setMuted(m_playerTracks[trackIndex], muted);
}

/// @brief 查询正式玩家区域的逐轨静音。
/// @param trackIndex 固定控制数组索引。
/// @return 越界时返回 false。
bool KeySoundControlBank::isPlayerTrackMuted(
    std::uint32_t trackIndex) const noexcept
{
    return trackIndex < KEY_SOUND_TRACK_LIMIT
               ? load(m_playerTracks[trackIndex]).muted
               : false;
}

/// @brief 更新正式玩家区域的逐轨增益。
/// @param trackIndex 固定控制数组索引。
/// @param gain 将被量化的线性增益。
void KeySoundControlBank::setPlayerTrackGain(std::uint32_t trackIndex,
                                             float         gain) noexcept
{
    if ( trackIndex >= KEY_SOUND_TRACK_LIMIT ) return;
    setGain(m_playerTracks[trackIndex], gain);
}

/// @brief 原子替换正式玩家轨的完整控制值。
/// @param trackIndex 固定控制数组索引。
/// @param control 同时提交的静音与增益。
void KeySoundControlBank::setPlayerTrackControl(
    std::uint32_t trackIndex, const KeySoundControlSnapshot& control) noexcept
{
    if ( trackIndex >= KEY_SOUND_TRACK_LIMIT ) return;
    // 完整 store 用于需要 mute 与 gain 同时切换的 UI 操作，避免中间组合可见。
    m_playerTracks[trackIndex].packed.store(pack(control),
                                            std::memory_order_relaxed);
}

/// @brief 查询正式玩家轨道增益。
/// @param trackIndex 固定控制数组索引。
/// @return 越界时返回单位增益。
float KeySoundControlBank::getPlayerTrackGain(
    std::uint32_t trackIndex) const noexcept
{
    return trackIndex < KEY_SOUND_TRACK_LIMIT
               ? load(m_playerTracks[trackIndex]).gain
               : 1.0F;
}

/// @brief 查询正式玩家轨道完整控制。
/// @param trackIndex 固定控制数组索引。
/// @return 越界时返回单位控制。
KeySoundControlSnapshot KeySoundControlBank::getPlayerTrackControl(
    std::uint32_t trackIndex) const noexcept
{
    return trackIndex < KEY_SOUND_TRACK_LIMIT ? load(m_playerTracks[trackIndex])
                                              : KeySoundControlSnapshot{};
}

/// @brief 更新草稿区域总静音。
/// @param muted 新静音状态。
void KeySoundControlBank::setDraftAreaMuted(bool muted) noexcept
{
    setMuted(m_draftArea, muted);
}

/// @brief 查询草稿区域总静音。
/// @return 当前完整快照中的静音位。
bool KeySoundControlBank::isDraftAreaMuted() const noexcept
{
    return load(m_draftArea).muted;
}

void KeySoundControlBank::setDraftTrackMuted(std::uint32_t trackIndex,
                                             bool          muted) noexcept
{
    // 草稿区使用独立固定数组，不与正式玩家区的同索引轨道共享状态。
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

/// @brief 更新草稿区域的逐轨增益。
/// @param trackIndex 固定控制数组索引。
/// @param gain 将被量化的线性增益。
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

/// @brief 更新 BGM 区域总增益。
/// @param gain 将被量化的线性增益。
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
    // BGM 轨道索引相对玩家轨道区，但控制存储与玩家打击音完全分离。
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

/// @brief 更新 BGM 区域的逐轨增益。
/// @param trackIndex 固定控制数组索引。
/// @param gain 将被量化的线性增益。
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

/// @brief 更新 Bound 或 Unbound 类别增益。
/// @param group 目标音效类别。
/// @param gain 将被量化的线性增益。
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

/// @brief 合成玩家或草稿 Key 音实例的实时有效增益。
/// @param control 实例区域、轨道与类别信息。
/// @return 任一级静音为零，否则返回三级增益乘积。
/// @warning 音频热路径；只执行固定次数 relaxed 原子读取。
float KeySoundControlBank::effectivePlayerGain(
    const KeySoundPlaybackControl& control) const noexcept
{
    // 普通 SFX 没有启用 Key 音控制，必须旁路全部区域与类别状态。
    if ( !control.enabled ) return 1.0F;

    // 玩家区与草稿区只在此处选择一次，后续区域和逐轨读取保持同一侧。
    const bool isDraft = control.area == KeySoundPlaybackArea::Draft;
    const auto area    = isDraft ? load(m_draftArea) : load(m_playerArea);
    if ( area.muted ) return 0.0F;

    // 越界轨道保留默认单位快照，但仍服从区域与音效类别控制。
    KeySoundControlSnapshot track;
    if ( control.playerTrackIndex < KEY_SOUND_TRACK_LIMIT ) {
        track = isDraft ? load(m_draftTracks[control.playerTrackIndex])
                        : load(m_playerTracks[control.playerTrackIndex]);
        if ( track.muted ) return 0.0F;
    }

    // 类别控制跨玩家与草稿区域共享，用于统一 Bound/Unbound 响度。
    const auto group = load(m_effectGroups[groupIndex(control.effectGroup)]);
    if ( group.muted ) return 0.0F;
    return area.gain * track.gain * group.gain;
}

/// @brief 合成自动 BGM 采样片段的实时有效增益。
/// @param trackIndex BGM 区域内的相对轨道索引。
/// @return 区域与逐轨增益乘积，静音时为零。
/// @warning 音频热路径；最多读取两个固定原子槽。
float KeySoundControlBank::effectiveBgmTrackGain(
    std::uint32_t trackIndex) const noexcept
{
    // BGM 区总控优先短路，避免静音时额外读取逐轨原子。
    const auto area = load(m_bgmArea);
    if ( area.muted ) return 0.0F;

    // 没有可表示逐轨控制的异常索引仍服从 BGM 区域总增益。
    if ( trackIndex >= KEY_SOUND_TRACK_LIMIT ) return area.gain;
    const auto track = load(m_bgmTracks[trackIndex]);
    return track.muted ? 0.0F : area.gain * track.gain;
}

}  // namespace MMM::Audio
