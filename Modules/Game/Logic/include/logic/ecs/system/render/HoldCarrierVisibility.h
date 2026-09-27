#pragma once

#include "logic/ecs/components/NoteComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "mmm/note/HoldScrollSemantics.h"
#include <algorithm>

namespace MMM::Logic::System
{
/// @brief 对独立尾部 HS 的原始 seg 执行根载体可见性门禁。
/// @param note 根物件；其它类型及共享 HS 长条不增加过滤。
/// @param cache 当前视口使用的滚动缓存。
/// @param currentTime 当前动画时间，单位秒。
/// @param currentAbsY 当前动画时间的滚动坐标，包含动画缩放。
/// @param minDelta 视口相对距离下界，调用方已计入纹理边缘余量。
/// @param maxDelta 视口相对距离上界，与下界使用相同缩放空间。
/// @return 根载体进入时间窗口或空间窗口时允许后续独立端点绘制。
/// @note Malody 原始 seg 的节点 HS 影响几何，载体的显示区间仍共享头部 HS。
/// @note 负头部 HS 的区间按空间上下界规整，兼容编辑器既有反向可见性规则。
/// @warning 快照及框选热路径：只查询三个缓存时间点，不分配、不扫描实体。
inline bool isIndependentHoldCarrierVisible(const NoteComponent& note,
                                            const ScrollCache&   cache,
                                            double               currentTime,
                                            double currentAbsY, double minDelta,
                                            double maxDelta)
{
    if ( note.m_type != ::MMM::NoteType::HOLD ||
         ::MMM::holdEndHsAnchor(note.m_metadata,
                                note.m_timestamp,
                                note.m_duration) == note.m_timestamp ) {
        return true;
    }
    const double endTime = note.m_timestamp + note.m_duration;
    // 沿用已有载体的判定时间余量；这是非阻塞的相交判断，不是等待窗口。
    // 停卷轴可能使很早或很晚的音符仍在视口内，不能仅按秒数截断生命周期。
    if ( note.m_timestamp <= currentTime + 0.1 && endTime >= currentTime - 0.1 )
        return true;
    // 此处两端必须共用头部 HS；分别取 HS 会让异号端点在极远处永久跨屏。
    // 通过门禁后，主体长度和尾部拾取仍使用各自节点的 HS，不在这里改几何。
    const double start =
        cache.getDisplayDelta(note.m_timestamp, currentAbsY, note.m_timestamp);
    const double end =
        cache.getDisplayDelta(endTime, currentAbsY, note.m_timestamp);
    return std::min(start, end) <= maxDelta && std::max(start, end) >= minDelta;
}
}  // namespace MMM::Logic::System
