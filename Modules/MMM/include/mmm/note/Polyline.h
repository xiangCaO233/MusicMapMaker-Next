#pragma once

#include "mmm/note/Flick.h"
#include "mmm/note/Hold.h"
#include "mmm/note/Note.h"
#include <deque>

namespace MMM
{

class Polyline : public Note
{
public:
    Polyline() { m_type = NoteType::POLYLINE; }
    Polyline(Polyline&&)                 = default;
    Polyline(const Polyline&)            = default;
    Polyline& operator=(Polyline&&)      = default;
    Polyline& operator=(const Polyline&) = default;
    ~Polyline() override                 = default;


    /// @brief 所有子滑键引用(数据始终存储在map的notedata中)
    std::deque<std::reference_wrapper<Flick>> m_subFlicks;

    /// @brief 所有子长条引用(数据始终存储在map的notedata中)
    std::deque<std::reference_wrapper<Hold>> m_subHolds;

    /// @brief 所有子物件的序列
    ///
    /// @note
    /// 合法的Polyline必须是Flick和Hold交替存储的
    /// 且
    /// 合法的Polyline中的Hold的起始时间戳必须严格等于(如果有)前一个Flick的时间戳
    /// 合法的Polyline中的Flick的时间戳必须严格等于(如果有)前一个Hold的结束时间戳
    /// (即Flick.m_timestamp == Hold.m_timestamp + Hold.m_duration)
    ///
    /// 对于起始物件和结束物件Flick或Hold类型均合法
    /// Polyline的时间戳m_timestamp和轨道m_track是m_subNotes中第一个subNote的对应值
    ///
    /// 这里m_subNotes存的依旧只是map的notedata内部实际物件资源的引用
    std::deque<std::reference_wrapper<Note>> m_subNotes;
};

}  // namespace MMM
