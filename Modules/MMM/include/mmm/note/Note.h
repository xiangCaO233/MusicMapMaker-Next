#pragma once

#include "mmm/Metadata.h"
#include <cstdint>
#include <string>
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

    /// @brief 物件绑定的自定义音效资源标识；为空时使用内置打击音效。
    std::string m_boundSound;

    /// @brief 所有物件元数据
    NoteMetadata m_metadata;

    /// @brief 从osu描述加载
    virtual void from_osu_description(
        const std::vector<std::string>& description, int32_t orbit_count);

    /// @brief 转换为osu描述
    virtual std::string to_osu_description(int32_t orbit_count);
};


}  // namespace MMM
