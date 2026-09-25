#pragma once

#include "common/render/RenderSnapshot.h"
#include <cmath>
#include <cstdint>

namespace MMM::Canvas
{
/// 编辑教程从正式 RenderSnapshot 中取几何，避免在 UI 线程读取 ECS。
/// 一次步骤只关心进入时的基线与当前已发布的结果。
/// 释放事件记录编辑入口所用的实体和部件，几何差异负责判定是否有效。
/// 两者缺一不可：相同部件的空拖动不能算完成，其它实体的编辑也不能借用。
/// 续接动作可替换根实体，令牌和源句柄分别校验新旧两端的来源。
/// 子段数组有固定容量，超出容量时只能验证数量变化，不能读取被截断尾部。
/// 渲染坐标不参与判定；滚动和变速只影响命中目标的位置提示。
/// @warning 这些规则每帧求值，保持常量规模且不分配临时容器。
/// @brief 创作演练中需要真实编辑结果的操作种类。
enum class ComposeEditStep {
    MoveNote,
    ResizeHold,
    ResizeFlick,
    MoveSubHold,
    MoveSubFlick,
    MergeSubNote,
    ExtendNote,
    ResumePolyline,
};

/// @brief 检查一次释放事务是否真正改变了指定练习物件。
/// @param step 当前演练操作；类型约束防止其它编辑冒充目标动作。
/// @param before 步骤进入时的正式几何，来自同一谱面实例的快照。
/// @param after 逻辑线程结束手势后发布的正式几何。
/// @param event 最近一次已结束的拖动或续接手势。
/// @param firstRevision 步骤开始时的事务序号，旧手势不能抵扣新步骤。
/// @return 原物件身份或继承后的槽位有效，且目标几何发生预期变化时为真。
/// @note 折线续接可能重建根实体，因此只要求步骤令牌不变并核对旧源实体。
/// @warning UI 每帧仅比较固定大小快照；不得查询 ECS 或等待逻辑线程锁。
inline bool composeEditSatisfied(
    ComposeEditStep                                                     step,
    const Common::Render::RenderSnapshot::WalkthroughPracticeNoteState& before,
    const Common::Render::RenderSnapshot::WalkthroughPracticeNoteState& after,
    const Common::Render::RenderSnapshot::WalkthroughEditEvent&         event,
    std::uint64_t firstRevision)
{
    using Common::Render::HoverPart;
    using Event = Common::Render::RenderSnapshot::WalkthroughEditEvent;
    // 步骤初始快照必须真的含有目标；仅有令牌但实体已被删除时不能练习。
    // 事务序号挡住进入步骤之前的动作；源实体挡住对别的练习物件的编辑。
    // after 的令牌可以跨根实体替换保留，不要求前后 entt 句柄相同。
    if ( !before.alive || !after.alive || before.token == 0 ||
         before.token != after.token || event.revision <= firstRevision ||
         event.sourceEntity != before.entity )
        return false;
    const bool       drag          = event.kind == Event::Kind::Drag;
    const bool       brush         = event.kind == Event::Kind::Brush;
    constexpr double timeTolerance = 1e-6;
    switch ( step ) {
    case ComposeEditStep::MoveNote:
        // 普通 Note 的移动既可跨轨也可跨拍；拖动头部是专用入口。
        // 比较根时间而不是当前屏幕 y，变速和滚动不会伪造位移。
        return drag && event.part == HoverPart::Head &&
               before.root.type == ::MMM::NoteType::NOTE &&
               after.root.type == ::MMM::NoteType::NOTE &&
               (before.root.track != after.root.track ||
                std::abs(before.root.timestamp - after.root.timestamp) >
                    timeTolerance);
    case ComposeEditStep::ResizeHold:
        // Hold 时长以尾部编辑为准，整体平移或头部拖动不会过关。
        // 容忍序列化与吸附计算中的微小浮点误差。
        return drag && event.part == HoverPart::HoldEnd &&
               before.root.type == ::MMM::NoteType::HOLD &&
               after.root.type == ::MMM::NoteType::HOLD &&
               std::abs(before.root.duration - after.root.duration) >
                   timeTolerance;
    case ComposeEditStep::ResizeFlick:
        // 箭头端点可在同一拍位内改轨，时间变化不是滑动距离。
        // dtrack 是有符号轨差，向左与向右都直接比较原值。
        return drag && event.part == HoverPart::FlickArrow &&
               before.root.type == ::MMM::NoteType::FLICK &&
               after.root.type == ::MMM::NoteType::FLICK &&
               before.root.dtrack != after.root.dtrack;
    case ComposeEditStep::MoveSubHold:
        // 第三子段是教程路线的中间 Hold；它的横向拖动带着后缀一起移动。
        // 数量不变意味着本步骤只练局部位置，结构合并留给后续练习。
        // 截断快照不能安全索引目标，因此先检查已捕获子段数。
        return drag && event.part == HoverPart::HoldBody &&
               event.subIndex == 2 && before.capturedSubNoteCount > 2 &&
               after.capturedSubNoteCount > 2 &&
               before.subNotes[2].type == ::MMM::NoteType::HOLD &&
               after.subNotes[2].track != before.subNotes[2].track &&
               after.subNoteCount == before.subNoteCount;
    case ComposeEditStep::MoveSubFlick:
        // 第四子段是内部 Flick；沿时间拖动会改它的起点与邻接 Hold。
        // 用户本步骤必须保留段数，避免提前把合并步骤的目标删掉。
        // 仍要校验拾取索引，因为多个子段共用 HoldBody 部件类型。
        return drag && event.part == HoverPart::HoldBody &&
               event.subIndex == 3 && before.capturedSubNoteCount > 3 &&
               after.capturedSubNoteCount > 3 &&
               before.subNotes[3].type == ::MMM::NoteType::FLICK &&
               std::abs(after.subNotes[3].timestamp -
                        before.subNotes[3].timestamp) > timeTolerance &&
               after.subNoteCount == before.subNoteCount;
    case ComposeEditStep::MergeSubNote:
        // 局部拖动触发清理后，正式子段数应减少。
        // 退化链可能让整条折线降级为独立 Hold，不能强制要求结果仍为折线。
        // 只检查源对象曾是折线，保证其它物件缩短不冒充内部合并。
        return drag && event.part == HoverPart::HoldBody &&
               event.subIndex > 0 && before.subNoteCount >= 3 &&
               after.subNoteCount < before.subNoteCount &&
               before.root.type == ::MMM::NoteType::POLYLINE;
    case ComposeEditStep::ExtendNote:
        // Shift 画笔从独立 Hold/Flick 尾部起笔，提交后生成新折线根。
        // 新根句柄可变；上方的源句柄与沿用的教学令牌保护身份链。
        // 至少两段防止清理后重新退化为原来的单段类型。
        return brush &&
               (before.root.type == ::MMM::NoteType::HOLD ||
                before.root.type == ::MMM::NoteType::FLICK) &&
               after.root.type == ::MMM::NoteType::POLYLINE &&
               after.subNoteCount >= 2;
    case ComposeEditStep::ResumePolyline:
        // 续写与回写都从现有折线出发，不能用新画一条无关折线抵扣。
        // 回写可以维持段数，但末段几何必须实质改变。
        if ( !brush || before.root.type != ::MMM::NoteType::POLYLINE ||
             after.root.type != ::MMM::NoteType::POLYLINE ||
             after.subNoteCount < 2 )
            return false;
        // 新增子段是最直接的续写证据，不要求尾段类型固定。
        // 数量减少属于擦除或清理，不视为本步骤的续写结果。
        if ( after.subNoteCount > before.subNoteCount ) return true;
        if ( after.subNoteCount != before.subNoteCount ||
             before.subNoteCount == 0 ||
             before.subNoteCount > before.capturedSubNoteCount ||
             after.subNoteCount > after.capturedSubNoteCount )
            return false;
        {
            // 尾段由同一位置的独立值比较，不依赖子实体在 ECS 中的句柄。
            // 根对象替换时子实体往往也重建，索引和几何比句柄更稳定。
            const auto& oldTail = before.subNotes[before.subNoteCount - 1];
            const auto& newTail = after.subNotes[after.subNoteCount - 1];
            return oldTail.type != newTail.type ||
                   oldTail.track != newTail.track ||
                   oldTail.dtrack != newTail.dtrack ||
                   std::abs(oldTail.timestamp - newTail.timestamp) >
                       timeTolerance ||
                   std::abs(oldTail.duration - newTail.duration) >
                       timeTolerance;
        }
    }
    return false;
}
}  // namespace MMM::Canvas
