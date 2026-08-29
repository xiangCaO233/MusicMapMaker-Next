#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace MMM::Audio
{

/// @brief Key 音逐轨控制支持的最大轨道数。
inline constexpr std::size_t KEY_SOUND_TRACK_LIMIT = 4096U;

/// @brief 不绑定玩家轨道的音效播放索引。
inline constexpr std::uint32_t KEY_SOUND_INVALID_TRACK_INDEX =
    std::numeric_limits<std::uint32_t>::max();

/// @brief 玩家物件打击音效使用的资源类别。
enum class KeySoundEffectGroup : std::uint8_t {
    Unbound,  ///< 未绑定项目音效文件，使用皮肤默认打击音效。
    Bound     ///< 已绑定项目音效文件。
};

/// @brief 单项 Key 音控制的完整只读快照。
struct KeySoundControlSnapshot {
    /// @brief 是否静音。
    bool muted{ false };

    /// @brief 线性增益，范围为 0.0~2.0。
    float gain{ 1.0F };
};

/// @brief 随单个预定打击音实例发布的运行时控制描述。
struct KeySoundPlaybackControl {
    /// @brief 是否对该播放实例应用 Key 音运行时控制。
    bool enabled{ false };

    /// @brief 玩家区零基轨道索引；无轨道控制时使用无效值。
    std::uint32_t playerTrackIndex{ KEY_SOUND_INVALID_TRACK_INDEX };

    /// @brief 当前物件使用的打击音效资源类别。
    KeySoundEffectGroup effectGroup{ KeySoundEffectGroup::Unbound };
};

}  // namespace MMM::Audio
