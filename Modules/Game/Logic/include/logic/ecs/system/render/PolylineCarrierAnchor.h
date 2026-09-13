#pragma once

#include "logic/ecs/components/NoteComponent.h"

#include <cmath>
#include <cstddef>

namespace MMM::Logic::System
{
/// @brief 返回 RMSlide 虚拟载体用于采样 HS 的起点时间。
/// @param note 已展开为 Hold/Flick 序列的折线。
/// @param index 合法子物件索引。
/// @return 长条末端的横向滑键继承该长条起点，其余载体使用自身起点。
/// @details rmslideEXF 的虚拟 type 12 同时绘制竖段和末端横段；
/// ProcessorFree.AppendExtraNotes 为该虚拟对象首尾使用同一 HS。
/// @warning 逐节点热路径，仅访问相邻元素，不分配、不扫描时间线。
inline double polylineCarrierAnchor(const NoteComponent& note,
                                    std::size_t          index)
{
    const auto& sub = note.m_subNotes[index];
    if ( index > 0 && sub.type == ::MMM::NoteType::FLICK ) {
        const auto& previous = note.m_subNotes[index - 1];
        // 只认相接的竖段末端，不能让非连续或独立滑键继承前一载体。
        if ( previous.type == ::MMM::NoteType::HOLD &&
             std::abs(previous.timestamp + previous.duration - sub.timestamp) <=
                 1e-7 ) {
            return previous.timestamp;
        }
    }
    // 下一条 Hold 是新的虚拟对象，必须重新采样 HS，不能统一到整条折线头部。
    return sub.timestamp;
}
}  // namespace MMM::Logic::System
