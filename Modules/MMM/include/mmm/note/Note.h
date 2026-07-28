#pragma once

#include "mmm/Metadata.h"
#include "mmm/sample/AudioSample.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace MMM
{

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

    /// @brief 物件命中时触发的可选采样绑定。
    std::optional<AudioSampleBinding> m_sampleBinding;

    /// @brief 旧版绑定音效字段；首轮迁移期间与 m_sampleBinding 保持兼容。
    std::string m_boundSound;

    /// @brief 所有物件元数据
    NoteMetadata m_metadata;

    /// @brief 设置物件命中采样并同步旧版字段。
    /// @param binding 待设置的采样绑定；资源标识为空时清除绑定。
    void setSampleBinding(AudioSampleBinding binding)
    {
        if ( binding.m_audioResourceId.empty() ) {
            clearSampleBinding();
            return;
        }
        m_boundSound    = binding.m_audioResourceId;
        m_sampleBinding = std::move(binding);
    }

    /// @brief 清除物件命中采样并同步旧版字段。
    void clearSampleBinding()
    {
        m_sampleBinding.reset();
        m_boundSound.clear();
    }

    /// @brief 获取兼容旧版字段后的物件命中采样。
    /// @return 有效采样绑定；没有绑定时返回空。
    [[nodiscard]] std::optional<AudioSampleBinding> getSampleBinding() const
    {
        if ( m_boundSound.empty() ) return std::nullopt;
        if ( m_sampleBinding &&
             m_sampleBinding->m_audioResourceId == m_boundSound ) {
            return m_sampleBinding;
        }
        return AudioSampleBinding{ m_boundSound, 1.0F };
    }

    /// @brief 从osu描述加载
    virtual void from_osu_description(
        const std::vector<std::string>& description, int32_t orbit_count);

    /// @brief 转换为osu描述
    virtual std::string to_osu_description(int32_t orbit_count);
};


}  // namespace MMM
