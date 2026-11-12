#pragma once

#include "common/render/NoteRenderData.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace MMM::Common
{
/// @brief 验证原生折线的正交、交替和端点连接契约。
/// @param segments 只借用父物件的子段，不包含独立 Note 或虚构锚点。
/// @param timestamp 父起点，必须与第一段相同。
/// @param track 父轨道，必须与第一段相同。
/// @param minimumTrack 当前玩家或草稿域的最小绝对轨号。
/// @param maximumTrack 当前域的最大绝对轨号，不允许跨域连接。
/// @return 每个子段和接缝均合法时返回真。
/// @details 合法定义来自 mmm/note/Polyline.h；Hold 负责时间，Flick 负责换轨。
/// 两种子段必须交替，下一段起点必须等于前一段终点。
/// 合法编辑结果和拒绝坏输入都采用这个完整契约，不能只比节点数量。
/// 不能以渲染出的连接线替代模型中的缺失 Hold/Flick。
/// @warning 仅编辑起笔、离散命令及回归断言调用；不逐帧重复扫描节点列表。
inline bool isCanonicalPolyline(
    std::span<const Render::PolylineSubNote> segments, double timestamp,
    int track, int minimumTrack, int maximumTrack)
{
    // 新建通常把单段降级为独立物件；旧批注折线仍可合法保存一个载体。
    // 这里验证持久化几何契约，不因容器未降级而禁用既有端点编辑。
    if ( segments.empty() || !std::isfinite(timestamp) || timestamp < 0 ||
         minimumTrack > maximumTrack )
        return false;
    double       endTime  = timestamp;
    std::int64_t endTrack = track;
    for ( std::size_t i = 0; i < segments.size(); ++i ) {
        const auto& segment = segments[i];
        // 同时核对时间和轨道，避免只检查交替类型却允许断口或斜线。
        // 容差仅吸收秒单位浮点运算，不会把可见间隙视为连续。
        if ( !std::isfinite(segment.timestamp) ||
             std::abs(segment.timestamp - endTime) > 1e-7 ||
             segment.trackIndex != endTrack || endTrack < minimumTrack ||
             endTrack > maximumTrack ||
             (i > 0 && segment.type == segments[i - 1].type) )
            return false;
        if ( segment.type == ::MMM::NoteType::HOLD ) {
            // Hold 在同一轨上前进，零时长只允许未提交预览暂存。
            if ( !std::isfinite(segment.duration) || segment.duration <= 0 )
                return false;
            endTime += segment.duration;
            if ( !std::isfinite(endTime) ) return false;
        } else if ( segment.type == ::MMM::NoteType::FLICK ) {
            // 横段只改变终轨，使用宽整数避免损坏输入的加法溢出。
            if ( segment.dtrack == 0 ) return false;
            endTrack += segment.dtrack;
            if ( endTrack < minimumTrack || endTrack > maximumTrack )
                return false;
        } else {
            // 普通 Note 是独立物件，不能作为自由点插进正交折线。
            return false;
        }
    }
    return true;
}
}  // namespace MMM::Common
