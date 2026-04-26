#pragma once

#include "mmm/note/Note.h"

namespace MMM
{

class Flick : public Note
{
public:
    Flick() { m_type = NoteType::FLICK; }
    Flick(Flick&&)                 = default;
    Flick(const Flick&)            = default;
    Flick& operator=(Flick&&)      = default;
    Flick& operator=(const Flick&) = default;
    ~Flick() override              = default;

    /// @brief 滑键滑动参数
    /// 代表从 m_track 轨道滑动到 m_track + m_dtrack 轨道处
    int32_t m_dtrack{ 1 };
};

}  // namespace MMM
