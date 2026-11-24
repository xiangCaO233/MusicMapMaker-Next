#include "common/render/ScrollRenderData.h"
/// @file BeatmapSession_TimingTable.cpp
/// @brief 投影独立于视口的时间点表格。
/// @details 已发布对象只保留原始时间、效果、数值和逻辑实体标识。
/// 表格消费期间只借用整代数据，实体依然通过命令入口校验；
/// 对话框打开顺序和渲染线程背压不会改写逻辑注册表。
/// 逻辑线程错过请求边沿时会在之后的活动 update 重试，无需 UI 重入逻辑层。
/// 新一代行的排序在发布前完成，避免 UI 等待排序或复制。
/// 即使已经缓存数据，也只有活动会话才允许回答 UI 的刷新意图。
#include "logic/BeatmapSession.h"
#include "logic/TimingTableSnapshot.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/ecs/system/ScrollCache.h"
#include "logic/session/context/SessionContext.h"
#include "mmm/timing/TimingEffect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>

namespace MMM::Logic
{

/// @brief 合并 UI 请求，并返回逻辑线程上次发布的表格。
/// @warning 表格可见时每帧读取。共享句柄保证编辑或切换谱面后旧行仍可使用。
std::shared_ptr<const TimingTableSnapshot>
BeatmapSession::requestTimingTableSnapshot()
{
    // 请求只唤起刷新；表格的数据与代际由发布槽同步。
    m_timingTableRequested.store(true, std::memory_order_relaxed);
    return m_timingTableSnapshot.load();
}

/// @brief 在逻辑线程投影完整 Timing 表格，在修订不变时复用已有发布。
/// @note 读模型仅由逻辑线程构造。它不携带画布几何或向模型回写的访问权。
/// @note 使用已完成的滚动缓存修订，确保表格和常规渲染使用同一轮 Timing。
/// @note 请求可合并；UI 不等待逻辑任务完成，载入时由命令撤回旧发布。
/// @note 活动谱面不需要打开主画布才能响应请求。空闲分支也会到达此入口。
/// @warning
/// 活动会话每轮只检查待处理标志。仅有请求且谱面或修订变化时遍历与排序。
void BeatmapSession::publishTimingTableSnapshot()
{
    if ( !m_timingTableRequested.load(std::memory_order_relaxed) ) return;

    const auto& context = *m_ctx;
    const auto  id =
        reinterpret_cast<std::uintptr_t>(context.currentBeatmap.get());
    const auto* cache =
        context.timelineRegistry.ctx().find<System::ScrollCache>();
    // Timing 改动先标记滚动缓存。保持请求直到缓存产生新修订，
    // 避免把尚未确认的修订保存为已经投影。
    // 会话空闲或相机暂时无法生成快照时，本轮不清除尚未满足的请求。
    if ( id != 0 && cache && cache->isDirty ) return;
    // 后续跨线程请求独立生成。如果它和本次交换竞争，由本轮或下一轮投影覆盖。
    m_timingTableRequested.exchange(false, std::memory_order_relaxed);

    // 承认修订只由唯一写者完成，重复请求无需重新取得发布的共享句柄。
    // 载入命令会清空记录，因此同一路径的重载也必须刷新表格。
    const auto revision = cache ? cache->getRevision() : 0;
    if ( m_timingTableRevision == revision ) return;

    // 先填满新容器再一次性发布，读者不会看到追加中的中间状态。
    auto next               = std::make_shared<TimingTableSnapshot>();
    next->beatmapInstanceId = id;
    // 空句柄表示仍待处理；零实例的完成结果表示会话确实没有活动谱面。
    if ( id == 0 ) {
        m_timingTableRevision = revision;
        m_timingTableSnapshot.store(std::move(next));
        return;
    }

    const auto& registry = context.timelineRegistry;
    const auto  view     = registry.view<const TimelineComponent>();
    auto&       rows     = next->rows;
    rows.reserve(view.size());

    // 每行映射唯一事件实体。事件几何保留默认值，表格只使用时间、效果与编辑身份。
    // 取当前完整组件视图才能包含屏幕外或卷轴折返处的真实事件。
    for ( auto entity : view ) {
        const auto& tc = view.get<const Logic::TimelineComponent>(entity);
        Common::Render::TimelineInteractiveElement el{};
        // registry 中一段就是一个实体，复制定义不会增加表格行。
        // 运行时虚拟积分点不属于这个 registry，也不进入选择和剪贴板。
        // 表格可用整体移动与删除，范围细调由专门窗口提交一次动作。
        el.interpolation = tc.m_interpolation;
        el.time          = tc.m_timestamp;
        el.y             = 0.0f;

        // 持有原始数值，显示单位和剪贴板换算继续由现有 UI 接口负责。
        // STOP 等实际事件仍保留自己的稳定实体，不共享同时间的其他效果身份。
        if ( tc.m_effect == ::MMM::TimingEffect::BPM ) {
            el.effects   = Common::Render::SCROLL_EFFECT_BPM;
            el.bpmEntity = entity;
            el.bpmValue  = tc.m_value;
        } else if ( tc.m_effect == ::MMM::TimingEffect::SCROLL ) {
            el.effects      = Common::Render::SCROLL_EFFECT_SCROLL;
            el.scrollEntity = entity;
            el.scrollValue  = tc.m_value;
        } else if ( tc.m_effect == ::MMM::TimingEffect::JUMP ) {
            el.effects    = Common::Render::SCROLL_EFFECT_JUMP;
            el.jumpEntity = entity;
            el.jumpValue  = tc.m_value;
        } else if ( tc.m_effect == ::MMM::TimingEffect::STOP ) {
            el.effects    = Common::Render::SCROLL_EFFECT_STOP;
            el.stopEntity = entity;
            el.stopValue  = tc.m_value;
        } else if ( tc.m_effect == ::MMM::TimingEffect::HS ) {
            el.effects  = Common::Render::SCROLL_EFFECT_HS;
            el.hsEntity = entity;
            el.hsValue  = tc.m_value;
        }

        rows.push_back(std::move(el));
    }

    // 精确时间是排序主键。相近但不同的时间仍有确定次序，
    // 同一时间的多条 Timing 再按效果归类，保持原登记顺序。
    std::stable_sort(
        rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            const bool aFinite = std::isfinite(a.time);
            const bool bFinite = std::isfinite(b.time);
            if ( aFinite != bFinite ) return aFinite;
            if ( aFinite && a.time != b.time ) return a.time < b.time;
            return a.effects < b.effects;
        });
    // 一代完整结果进入发布槽，后续修改不原地改变 UI 正在使用的旧行。
    // 将修订写在发布之前，只有逻辑写者读取它；UI 只使用与 rows
    // 同一对象内的身份。
    m_timingTableRevision = revision;
    m_timingTableSnapshot.store(std::move(next));
}

}  // namespace MMM::Logic
