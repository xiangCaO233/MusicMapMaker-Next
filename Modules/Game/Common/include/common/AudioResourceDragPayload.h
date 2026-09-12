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
/// @note 版本后缀用于阻止不同二进制布局的载荷被错误解释。
inline constexpr char AUDIO_RESOURCE_DRAG_PAYLOAD_TYPE[] =
    "MMM_AUDIO_RESOURCE_V1";

/// @brief 拖拽载荷内资源 ID 的最大字节数，包含末尾空字符。
/// @note 容量按 UTF-8 字节计算，不按 Unicode 字符数量计算。
inline constexpr std::size_t AUDIO_RESOURCE_DRAG_ID_CAPACITY = 256U;

/// @brief 项目音频资源拖入编辑画布时使用的固定布局载荷。
struct AudioResourceDragPayload {
    /// @brief 项目内稳定音频资源 ID，以空字符结尾。
    char m_audioResourceId[AUDIO_RESOURCE_DRAG_ID_CAPACITY]{};

    /// @brief 资源的 Main 或 Effect 类型。
    AudioTrackType m_audioTrackType{};
};

static_assert(std::is_trivially_copyable_v<AudioResourceDragPayload>);
// 标准布局与可平凡复制共同保证 ImGui 可以逐字节传递该结构。
static_assert(std::is_standard_layout_v<AudioResourceDragPayload>);

/// @brief 判断资源 ID 能否完整写入固定布局拖拽载荷。
/// @param audioResourceId 项目内稳定音频资源 ID。
/// @return ID 非空且无需截断时返回 true。
/// @note 严格小于容量，为结尾空字符预留一个字节。
[[nodiscard]] inline bool canStoreAudioResourceDragId(
    std::string_view audioResourceId) noexcept
{
    // 拒绝空 ID 和需要截断的 ID，避免拖放后解析到其他资源。
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
    // 固定布局协议不允许静默截断，失败由调用方取消拖放载荷。
    if ( !canStoreAudioResourceDragId(audioResourceId) ) {
        return std::nullopt;
    }

    // 数组已零初始化；仍显式写入终止符，使有效长度契约清晰可查。
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
/// @note 若外部载荷缺少终止符，视图最多覆盖整个固定数组，不越界读取。
[[nodiscard]] inline std::string_view audioResourceIdView(
    const AudioResourceDragPayload& payload) noexcept
{
    // 有界查找同时兼容正常 C 字符串和来自外部的不完整字节载荷。
    const auto* begin = payload.m_audioResourceId;
    const auto* end =
        std::find(begin, begin + AUDIO_RESOURCE_DRAG_ID_CAPACITY, '\0');
    return { begin, static_cast<std::size_t>(end - begin) };
}

}  // namespace MMM::Common
