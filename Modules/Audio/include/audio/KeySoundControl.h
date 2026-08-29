#pragma once

#include "audio/KeySoundTypes.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace MMM::Audio
{

/// @brief 固定容量、可供 UI、逻辑线程和音频线程并发访问的 Key 音控制库。
///
/// 每项控制使用单个无锁原子字同时发布 mute 与 gain，避免两个字段出现撕裂
/// 快照。增益以 16 位定点数保存；0.0~2.0 范围内的最大量化误差小于
/// 0.000031。
class KeySoundControlBank
{
public:
    /// @brief 构造默认全开且增益为 1 的控制库。
    KeySoundControlBank() noexcept = default;

    KeySoundControlBank(const KeySoundControlBank&)            = delete;
    KeySoundControlBank& operator=(const KeySoundControlBank&) = delete;
    KeySoundControlBank(KeySoundControlBank&&)                 = delete;
    KeySoundControlBank& operator=(KeySoundControlBank&&)      = delete;

    /// @brief 设置整个玩家打击音区的静音覆盖。
    void setPlayerAreaMuted(bool muted) noexcept;

    /// @brief 查询整个玩家打击音区的静音覆盖。
    [[nodiscard]] bool isPlayerAreaMuted() const noexcept;

    /// @brief 设置指定玩家轨道的静音覆盖。
    void setPlayerTrackMuted(std::uint32_t trackIndex, bool muted) noexcept;

    /// @brief 查询指定玩家轨道的静音覆盖。
    [[nodiscard]] bool isPlayerTrackMuted(
        std::uint32_t trackIndex) const noexcept;

    /// @brief 设置指定玩家轨道的线性增益。
    void setPlayerTrackGain(std::uint32_t trackIndex, float gain) noexcept;

    /// @brief 原子发布指定玩家轨道的完整控制快照。
    /// @param trackIndex 零基玩家轨道索引。
    /// @param control 将在一次原子写入中同时替换 mute 与 gain。
    void setPlayerTrackControl(std::uint32_t                  trackIndex,
                               const KeySoundControlSnapshot& control) noexcept;

    /// @brief 查询指定玩家轨道的线性增益。
    [[nodiscard]] float getPlayerTrackGain(
        std::uint32_t trackIndex) const noexcept;

    /// @brief 原子读取指定玩家轨道的完整控制快照。
    /// @return 越界时返回未静音且增益为 1 的单位控制。
    [[nodiscard]] KeySoundControlSnapshot getPlayerTrackControl(
        std::uint32_t trackIndex) const noexcept;

    /// @brief 设置整个草稿打击音区的静音覆盖。
    void setDraftAreaMuted(bool muted) noexcept;

    /// @brief 查询整个草稿打击音区的静音覆盖。
    [[nodiscard]] bool isDraftAreaMuted() const noexcept;

    /// @brief 设置指定草稿轨道的静音覆盖。
    void setDraftTrackMuted(std::uint32_t trackIndex, bool muted) noexcept;

    /// @brief 查询指定草稿轨道的静音覆盖。
    [[nodiscard]] bool isDraftTrackMuted(
        std::uint32_t trackIndex) const noexcept;

    /// @brief 设置指定草稿轨道的线性增益。
    void setDraftTrackGain(std::uint32_t trackIndex, float gain) noexcept;

    /// @brief 查询指定草稿轨道的线性增益。
    [[nodiscard]] float getDraftTrackGain(
        std::uint32_t trackIndex) const noexcept;

    /// @brief 设置整个 BGM Key 音区的静音覆盖。
    void setBgmAreaMuted(bool muted) noexcept;

    /// @brief 查询整个 BGM Key 音区的静音覆盖。
    [[nodiscard]] bool isBgmAreaMuted() const noexcept;

    /// @brief 设置整个 BGM Key 音区的线性增益。
    void setBgmAreaGain(float gain) noexcept;

    /// @brief 查询整个 BGM Key 音区的线性增益。
    [[nodiscard]] float getBgmAreaGain() const noexcept;

    /// @brief 设置指定 BGM 轨道的静音覆盖。
    void setBgmTrackMuted(std::uint32_t trackIndex, bool muted) noexcept;

    /// @brief 查询指定 BGM 轨道的静音覆盖。
    [[nodiscard]] bool isBgmTrackMuted(std::uint32_t trackIndex) const noexcept;

    /// @brief 设置指定 BGM 轨道的线性增益。
    void setBgmTrackGain(std::uint32_t trackIndex, float gain) noexcept;

    /// @brief 查询指定 BGM 轨道的线性增益。
    [[nodiscard]] float getBgmTrackGain(
        std::uint32_t trackIndex) const noexcept;

    /// @brief 设置指定打击音效类别的静音覆盖。
    void setEffectGroupMuted(KeySoundEffectGroup group, bool muted) noexcept;

    /// @brief 查询指定打击音效类别的静音覆盖。
    [[nodiscard]] bool isEffectGroupMuted(
        KeySoundEffectGroup group) const noexcept;

    /// @brief 设置指定打击音效类别的线性增益。
    void setEffectGroupGain(KeySoundEffectGroup group, float gain) noexcept;

    /// @brief 查询指定打击音效类别的线性增益。
    [[nodiscard]] float getEffectGroupGain(
        KeySoundEffectGroup group) const noexcept;

    /// @brief 读取玩家或草稿预定打击音实例的最终运行时控制增益。
    /// @param control 实例所属区域、轨道和音效类别。
    /// @return 未启用控制时返回 1；任一级静音时返回 0。
    /// @warning 音频回调热路径：每个活动实例每个 block 调用一次，只执行
    /// 常量次 relaxed 原子读取与算术，禁止加入锁、分配或容器查询。
    [[nodiscard]] float effectivePlayerGain(
        const KeySoundPlaybackControl& control) const noexcept;

    /// @brief 读取 BGM 自动采样片段的最终运行时轨道增益。
    /// @param trackIndex 相对玩家轨道区的零基 BGM 轨道索引。
    /// @return BGM 区和逐轨控制的乘积；任一级静音时返回 0。
    /// @warning 音频回调热路径：每个活动片段每个 block 调用一次，只执行
    /// 两次 relaxed 原子读取与常量时间算术，禁止加入锁、分配或容器查询。
    [[nodiscard]] float effectiveBgmTrackGain(
        std::uint32_t trackIndex) const noexcept;

private:
    /// @brief 一个原子字内的量化增益位数。
    static constexpr std::uint32_t GAIN_QUANTIZED_MASK = 0x0000FFFFU;

    /// @brief 一个原子字内的静音标志位。
    static constexpr std::uint32_t MUTED_FLAG = 0x00010000U;

    /// @brief 默认 1.0 增益对应的量化值。
    static constexpr std::uint32_t DEFAULT_PACKED_CONTROL = 0x00008000U;

    /// @brief 单个完整发布的控制原子字。
    struct AtomicControl {
        /// @brief 同时包含静音标志和 16 位定点增益。
        /// @warning UI 或逻辑线程写，逻辑或音频线程读；单个原子字用于保证
        /// mute 与 gain 快照一致，relaxed 顺序只要求取得某个完整值。
        std::atomic<std::uint32_t> packed{ DEFAULT_PACKED_CONTROL };
    };

    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    /// @brief 将输入增益限制并量化到原子字。
    [[nodiscard]] static std::uint32_t quantizeGain(float gain) noexcept;

    /// @brief 将完整控制快照编码到单个原子字。
    [[nodiscard]] static std::uint32_t pack(
        const KeySoundControlSnapshot& control) noexcept;

    /// @brief 从原子字还原控制快照。
    [[nodiscard]] static KeySoundControlSnapshot unpack(
        std::uint32_t packed) noexcept;

    /// @brief 原子更新单项静音位并保留增益。
    static void setMuted(AtomicControl& control, bool muted) noexcept;

    /// @brief 原子更新单项增益并保留静音位。
    static void setGain(AtomicControl& control, float gain) noexcept;

    /// @brief 读取单项完整控制快照。
    [[nodiscard]] static KeySoundControlSnapshot load(
        const AtomicControl& control) noexcept;

    /// @brief 将类别枚举转换为固定数组索引。
    [[nodiscard]] static std::size_t groupIndex(
        KeySoundEffectGroup group) noexcept;

    /// @brief 玩家打击音区总控制。
    AtomicControl m_playerArea;

    /// @brief 玩家区逐轨控制。
    std::array<AtomicControl, KEY_SOUND_TRACK_LIMIT> m_playerTracks{};

    /// @brief 草稿打击音区总控制。
    AtomicControl m_draftArea;

    /// @brief 草稿区逐轨控制。
    std::array<AtomicControl, KEY_SOUND_TRACK_LIMIT> m_draftTracks{};

    /// @brief BGM Key 音区总控制。
    AtomicControl m_bgmArea;

    /// @brief BGM 区逐轨控制。
    std::array<AtomicControl, KEY_SOUND_TRACK_LIMIT> m_bgmTracks{};

    /// @brief 未绑定与绑定打击音效的类别控制。
    std::array<AtomicControl, 2U> m_effectGroups{};
};

}  // namespace MMM::Audio
