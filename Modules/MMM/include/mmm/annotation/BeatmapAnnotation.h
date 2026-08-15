#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace MMM
{

/// @brief 单条批注 Markdown 正文允许的最大 UTF-8 字节数。
inline constexpr std::size_t MAX_BEATMAP_ANNOTATION_CONTENT_BYTES = 8192U;

/// @brief 单张谱面允许保存的批注数量上限。
inline constexpr std::size_t MAX_BEATMAP_ANNOTATION_COUNT = 4096U;

/// @brief 批注及目标稳定标识允许的最大字节数。
inline constexpr std::size_t MAX_BEATMAP_ANNOTATION_ID_BYTES = 128U;

/// @brief 批注人 Creator 名称允许的最大 UTF-8 字节数。
inline constexpr std::size_t MAX_BEATMAP_ANNOTATION_AUTHOR_BYTES = 192U;

/// @brief 批注所依附的谱面位置类型。
enum class BeatmapAnnotationTargetKind : std::uint8_t {
    /// @brief 独立时间戳，不依附具体物件。
    TIMESTAMP,

    /// @brief 玩家轨道中的普通、长条、滑键或折线物件。
    PLAYER_OBJECT,

    /// @brief BGM 区中的自动采样物件。
    AUDIO_SAMPLE,
};

/// @brief 可同步、可持久化的单条谱面批注。
struct BeatmapAnnotation {
    /// @brief 批注自身的稳定协作标识。
    std::string m_id;

    /// @brief 批注所依附的位置类型。
    BeatmapAnnotationTargetKind m_targetKind{
        BeatmapAnnotationTargetKind::TIMESTAMP
    };

    /// @brief 物件目标的稳定协作标识；时间戳批注为空。
    std::string m_targetId;

    /// @brief 目标缺失时的回退时间，单位毫秒。
    double m_timestamp{ 0.0 };

    /// @brief 创建批注时记录的 Creator 名称。
    std::string m_author;

    /// @brief 批注的 Markdown 正文。
    std::string m_content;

    /// @brief 比较两条批注的全部持久化字段。
    bool operator==(const BeatmapAnnotation&) const = default;
};

}  // namespace MMM
