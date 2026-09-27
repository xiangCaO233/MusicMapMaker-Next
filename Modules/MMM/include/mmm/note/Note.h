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
    /// @brief 普通瞬时物件。
    NOTE,
    /// @brief 具有持续时间的长条物件。
    HOLD,
    /// @brief 带横向滑动方向的瞬时物件。
    FLICK,
    /// @brief 由多个节点组成的折线物件。
    POLYLINE,
};

/// @brief 所有玩家物件共享的值语义基类。
/// @details
/// 保存跨格式共有的时间、轨道、采样、注释和协作身份；来源格式特有字段由
/// NoteMetadata 承载。派生类只扩展自身几何，格式转换通过虚函数复用统一入口。
class Note
{
public:
    /// @brief 构造默认普通物件。
    Note();
    /// @brief 移动构造完整物件值。
    Note(Note&&) = default;
    /// @brief 复制构造完整物件值。
    Note(const Note&) = default;
    /// @brief 移动赋值完整物件值。
    Note& operator=(Note&&) = default;
    /// @brief 复制赋值完整物件值。
    Note& operator=(const Note&) = default;
    /// @brief 为派生物件提供多态销毁入口。
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

    /// @brief 从 osu!mania HitObject 字段加载普通物件。
    /// @param description 按逗号切分的 HitObject 字段。
    /// @param orbit_count 目标玩家轨道数。
    virtual void from_osu_description(
        const std::vector<std::string>& description, int32_t orbit_count);

    /// @brief 转换为 osu!mania 普通 HitObject 描述。
    /// @param orbit_count 用于把内部轨道映射回 0 至 512 的轨道坐标。
    /// @return 可写入 `[HitObjects]` 的单行字段。
    virtual std::string to_osu_description(int32_t orbit_count);
};


}  // namespace MMM
