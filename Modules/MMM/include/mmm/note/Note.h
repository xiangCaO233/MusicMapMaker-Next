#pragma once

#include "mmm/Metadata.h"
#include "mmm/sample/AudioSample.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace MMM
{

/// @brief 单个玩家物件注释允许保存和同步的最大 UTF-8 字节数。
inline constexpr std::size_t MAX_NOTE_ANNOTATION_BYTES = 8192U;

enum class NoteType {
    NOTE,
    HOLD,
    FLICK,
    POLYLINE,
};

class Note
{
public:
    Note();
    Note(Note&&)                 = default;
    Note(const Note&)            = default;
    Note& operator=(Note&&)      = default;
    Note& operator=(const Note&) = default;
    virtual ~Note();

    /// @brief 物件类型
    NoteType m_type{ NoteType::NOTE };

    /// @brief 物件时间戳
    double m_timestamp{ 0 };

    /// @brief 轨道位置索引
    uint32_t m_track{ 0 };

    /// @brief 是否为子物件（隶属于 Polyline）
    bool m_isSubNote{ false };

    /// @brief 物件命中时触发的可选采样绑定，是绑定状态的唯一权威来源。
    std::optional<AudioSampleBinding> m_sampleBinding;

    /// @brief 所有物件元数据。
    NoteMetadata m_metadata;

    /// @brief 编辑器内的协作注释；原始游戏格式导出时可忽略该字段。
    std::string m_annotation;

    /// @brief 协作会话内稳定的逻辑物件标识；普通谱面格式不会持久化该字段。
    std::string m_collaborationId;

    /// @brief 设置物件命中采样。
    /// @param binding 待设置的采样绑定；资源标识为空时清除绑定。
    void setSampleBinding(AudioSampleBinding binding)
    {
        if ( binding.m_audioResourceId.empty() ) {
            clearSampleBinding();
            return;
        }
        m_sampleBinding = std::move(binding);
    }

    /// @brief 清除物件命中采样。
    void clearSampleBinding() { m_sampleBinding.reset(); }

    /// @brief 获取物件命中采样。
    /// @return 有效采样绑定；没有绑定时返回空。
    [[nodiscard]] const std::optional<AudioSampleBinding>&
    getSampleBinding() const
    {
        return m_sampleBinding;
    }

    /// @brief 从osu描述加载
    virtual void from_osu_description(
        const std::vector<std::string>& description, int32_t orbit_count);

    /// @brief 转换为osu描述
    virtual std::string to_osu_description(int32_t orbit_count);
};


}  // namespace MMM
