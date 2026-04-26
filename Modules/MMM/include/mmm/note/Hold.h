#pragma once

#include "mmm/note/Note.h"

namespace MMM
{

class Hold : public Note
{
public:
    Hold() { m_type = NoteType::HOLD; }
    Hold(Hold&&)                 = default;
    Hold(const Hold&)            = default;
    Hold& operator=(Hold&&)      = default;
    Hold& operator=(const Hold&) = default;
    ~Hold() override             = default;

    /// @brief 长条持续时间
    double m_duration{ .0 };

    /// @brief 从osu描述加载
    void from_osu_description(const std::vector<std::string>& description,
                              int32_t orbit_count) override;
    /// @brief 转换为osu描述
    std::string to_osu_description(int32_t orbit_count) override;
};

}  // namespace MMM
