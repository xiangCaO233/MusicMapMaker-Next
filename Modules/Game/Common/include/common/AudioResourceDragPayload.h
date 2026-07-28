#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>

namespace MMM
{
enum class AudioTrackType;
}

namespace MMM::Common
{

/// @brief ImGui 项目音频资源拖拽载荷的稳定类型名。
inline constexpr char AUDIO_RESOURCE_DRAG_PAYLOAD_TYPE[] =
    "MMM_AUDIO_RESOURCE_V1";

/// @brief 拖拽载荷内资源 ID 的最大字节数，包含末尾空字符。
inline constexpr std::size_t AUDIO_RESOURCE_DRAG_ID_CAPACITY = 256U;

/// @brief 项目音频资源拖入编辑画布时使用的固定布局载荷。
struct AudioResourceDragPayload {
    /// @brief 项目内稳定音频资源 ID，以空字符结尾。
    char m_audioResourceId[AUDIO_RESOURCE_DRAG_ID_CAPACITY]{};

    /// @brief 资源的 Main 或 Effect 类型。
    AudioTrackType m_audioTrackType{};
};

static_assert(std::is_trivially_copyable_v<AudioResourceDragPayload>);
static_assert(std::is_standard_layout_v<AudioResourceDragPayload>);

/// @brief 判断资源 ID 能否完整写入固定布局拖拽载荷。
/// @param audioResourceId 项目内稳定音频资源 ID。
/// @return ID 非空且无需截断时返回 true。
[[nodiscard]] inline bool canStoreAudioResourceDragId(
    std::string_view audioResourceId) noexcept
{
    return !audioResourceId.empty() &&
           audioResourceId.size() < AUDIO_RESOURCE_DRAG_ID_CAPACITY;
}

/// @brief 构造可由 ImGui 逐字节复制的项目音频资源载荷。
/// @param audioResourceId 项目内稳定音频资源 ID。
/// @param audioTrackType 音频资源类型。
/// @return ID 可完整保存时返回载荷，否则返回空。
[[nodiscard]] inline std::optional<AudioResourceDragPayload>
makeAudioResourceDragPayload(std::string_view audioResourceId,
                             AudioTrackType   audioTrackType) noexcept
{
    if ( !canStoreAudioResourceDragId(audioResourceId) ) {
        return std::nullopt;
    }

    AudioResourceDragPayload payload;
    std::copy_n(audioResourceId.data(),
                audioResourceId.size(),
                payload.m_audioResourceId);
    payload.m_audioResourceId[audioResourceId.size()] = '\0';
    payload.m_audioTrackType                          = audioTrackType;
    return payload;
}

/// @brief 从固定布局载荷读取完整且有效的资源 ID。
/// @param payload 项目音频资源拖拽载荷。
/// @return 不包含末尾空字符的资源 ID 视图。
[[nodiscard]] inline std::string_view audioResourceIdView(
    const AudioResourceDragPayload& payload) noexcept
{
    const auto* begin = payload.m_audioResourceId;
    const auto* end =
        std::find(begin, begin + AUDIO_RESOURCE_DRAG_ID_CAPACITY, '\0');
    return { begin, static_cast<std::size_t>(end - begin) };
}

}  // namespace MMM::Common
