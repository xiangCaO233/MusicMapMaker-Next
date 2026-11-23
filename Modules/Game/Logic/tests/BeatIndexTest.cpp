#include "logic/session/SessionUtils.h"

#include "common/render/BeatTimeIndex.h"
#include "common/render/RenderSnapshot.h"
#include "log/colorful-log.h"
#include "logic/ecs/components/TimelineComponent.h"
#include "logic/session/context/SessionContext.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{

/// @brief 创建测试所需的 BPM 时间线组件。
/// @param timestamp BPM 红线时间，单位秒。
/// @param bpm 红线定义的 BPM。
/// @return 可直接加入有序 BPM 事件列表的组件。
/// @note 这里直接构造秒单位的 ECS 组件，不经过毫秒单位的谱面导入转换。
MMM::Logic::TimelineComponent makeBpm(double timestamp, double bpm)
{
    return { timestamp, MMM::TimingEffect::BPM, bpm, {} };
}

/// @brief 验证单个 BPM 段内仍按完整拍时长递增拍号。
/// @return 起始拍及下一拍的拍号均正确时返回 true。
/// @note 同时检查边界前和边界上，区分拍内取整错误与整体编号偏移。
/// @note 逻辑编号用于实体分拍线，独立于格式化索引的文本表现。
bool testBeatIndexWithinSingleBpmSegment()
{
    // 将首 BPM 放在非零时刻，防止实现错误地以歌曲零时刻作为拍号原点。
    const auto bpm = makeBpm(10.0, 120.0);
    // 列表借用栈上组件；组件在全部同步查询结束前保持有效。
    const std::vector<const MMM::Logic::TimelineComponent*> bpmEvents{ &bpm };

    // 120 BPM 每拍 0.5 秒，10.49 仍在第一拍，不能提前四舍五入到第二拍。
    const int firstBeat =
        MMM::Logic::SessionUtils::calculateBeatIndex(10.49, bpmEvents, 120.0);
    // 10.5 恰好落在下一拍起点，拍号从 1 开始而非从 0 开始。
    const int secondBeat =
        MMM::Logic::SessionUtils::calculateBeatIndex(10.5, bpmEvents, 120.0);
    if ( firstBeat != 1 || secondBeat != 2 ) {
        XERROR("Single BPM segment beat indexes are incorrect: {}, {}",
               firstBeat,
               secondBeat);
        return false;
    }
    return true;
}

/// @brief 验证不足一拍的旧 BPM 段在新红线开始时仍占用一个拍号。
/// @return 每条连续红线均推进拍号时返回 true。
/// @note BPM 变化会重新建立拍位相位，不能将旧段小数拍长延续到新段。
/// @note 对比分拍显示的舍入规则，逻辑拍线需要确保新段有独立编号。
bool testPartialBeatSegmentsAdvanceAtNextBpm()
{
    // 前两段各长 0.1 秒，分别不足 120 BPM 和 180 BPM 下的一整拍。
    // 用连续两个短段暴露逐段向下取整后拍号一直不推进的错误。
    const auto first  = makeBpm(10.0, 120.0);
    const auto second = makeBpm(10.1, 180.0);
    const auto third  = makeBpm(10.2, 240.0);
    // 显式按时间排列，测试不要求被测函数承担事件排序职责。
    const std::vector<const MMM::Logic::TimelineComponent*> bpmEvents{ &first,
                                                                       &second,
                                                                       &third };

    // 查询直接使用红线自身时间，确保检查的是段落归属边界而非附近采样点。
    const int secondIndex = MMM::Logic::SessionUtils::calculateBeatIndex(
        second.m_timestamp, bpmEvents, 120.0);
    const int thirdIndex = MMM::Logic::SessionUtils::calculateBeatIndex(
        third.m_timestamp, bpmEvents, 120.0);
    // 每个正长度短段占一个编号，所以两次切换依次进入第 2、3 拍。
    if ( secondIndex != 2 || thirdIndex != 3 ) {
        XERROR("Partial BPM segments did not advance beat indexes: {}, {}",
               secondIndex,
               thirdIndex);
        return false;
    }
    return true;
}

/// @brief 验证恰好整拍结束的 BPM 段不会因向上取整重复增加拍号。
/// @return 新红线恰好从第二拍开始时返回 true。
/// @note 与短段用例互补，避免通过对每个段落无条件加一来修复短段累计。
/// @note 取整规则按上一段有效拍长累计，切换后的 BPM 不能追溯影响旧分段。
bool testWholeBeatSegmentDoesNotOvercount()
{
    // 第一段恰好长一拍，不应把向上取整的边界容差解释成额外一拍。
    const auto first  = makeBpm(0.0, 120.0);
    const auto second = makeBpm(0.5, 180.0);
    const std::vector<const MMM::Logic::TimelineComponent*> bpmEvents{
        &first, &second
    };

    // 新 BPM 只决定新段的拍长，不能用于计算已经结束的旧段长度。
    const int beatIndex = MMM::Logic::SessionUtils::calculateBeatIndex(
        second.m_timestamp, bpmEvents, 120.0);
    // 旧段贡献 1，新段从自身第 1 拍开始，累计编号应为 2 而不是 3。
    if ( beatIndex != 2 ) {
        XERROR("Whole BPM segment overcounted beat index: {}", beatIndex);
        return false;
    }
    return true;
}

/// @brief 验证渲染拍号索引在普通轮次复用，并在 BPM 编辑后只替换一次。
/// @return 同刻归并、逐段累计和旧快照存活均符合预期时返回真。
/// @details 直接使用会话的时间线缓存及渲染复用入口来观察修订身份。
/// 用 8192 个节点放大普通轮次误重建的影响，并用两份快照检查旧修订的寿命。
/// UI 单独测试查询时使用的分拍数，因此本测试不构造窗口或用户配置。
bool testBeatTimeIndexRevision()
{
    using MMM::Logic::TimelineComponent;
    using MMM::Logic::SessionUtils::ensureBpmEvents;
    // 在测试线程构建注册表，避免编辑器音频和渲染后台影响缓存生命周期。
    // 工作槽和上一帧读槽分别持有自己的修订，正好对应生产端与消费端的边界。
    MMM::Logic::SessionContext context;
    auto&                      registry    = context.timelineRegistry;
    constexpr std::size_t      POINT_COUNT = 8192U;
    // 每个时刻间隔正好一拍。另加同刻和微小时差节点，检查归并规则。
    // 同刻先提供两个不同 BPM，其中一个累计零拍，另一个累计一拍。
    // 若稳定排序后的覆盖顺序不一致，首段的累计误差会传播到尾节点。
    const auto start = registry.create();
    registry.emplace<TimelineComponent>(start, makeBpm(-0.5, 30.0));
    for ( std::size_t index = 0; index < POINT_COUNT; ++index ) {
        registry.emplace<TimelineComponent>(registry.create(),
                                            makeBpm(-0.5 + 0.5 * index, 120.0));
    }
    // 相差半微秒的节点应舍弃；它不能覆盖较早节点的 BPM。
    registry.emplace<TimelineComponent>(registry.create(),
                                        makeBpm(-0.4999995, 90.0));
    ensureBpmEvents(context);
    // 首次 ensure 应完成整个发布；消费者不能等第二轮才看见索引。
    if ( !context.m_beatTimeIndex ) return false;

    // 同刻值沿用收集顺序中最后应用的组件。用会话缓存顺序作为独立见证，
    // 避免对 entt 实体分配和遍历的方向作隐含假设。
    const double firstBpm = context.bpmEvents[1]->m_value;
    // 后续每段恰好为一拍，可从谱面构造规则独立得出尾节点的累计值。
    const std::int64_t firstSegmentBeats = firstBpm == 120.0 ? 1 : 0;
    const auto&        points            = context.m_beatTimeIndex->m_points;
    if ( points.size() != POINT_COUNT || points.front().m_time != -0.5 ||
         points.front().m_bpm != firstBpm ||
         points.back().m_completedBeats !=
             static_cast<std::int64_t>(POINT_COUNT - 2) + firstSegmentBeats ) {
        XERROR("Beat formatting index has an unexpected node or prefix");
        return false;
    }

    // 两个真实快照槽分别代表已经发布的旧画面和更新后待发布的新画面。
    MMM::Common::Render::RenderSnapshot oldSnapshot;
    MMM::Common::Render::RenderSnapshot nextSnapshot;
    oldSnapshot.clear(context.m_beatTimeIndex);
    const auto* const firstIndex    = oldSnapshot.m_beatTimeIndex.get();
    const auto        firstRevision = context.bpmEventsRevision;
    // 只在测试读共享计数，用它确认普通复用没有残留额外的所有权。
    const auto ownerCount = oldSnapshot.m_beatTimeIndex.use_count();
    // 刻意复用同一个槽多次，验证没有因播放快照推进重建或增加持有者。
    for ( int update = 0; update < 128; ++update ) {
        ensureBpmEvents(context);
        oldSnapshot.clear(context.m_beatTimeIndex);
        // 修订号检测重建次数，地址检测快照挂接的对象是否被换掉。
        if ( context.bpmEventsRevision != firstRevision ||
             oldSnapshot.m_beatTimeIndex.get() != firstIndex ||
             oldSnapshot.m_beatTimeIndex.use_count() != ownerCount )
            return false;
    }

    // 编辑归并后确定生效的同刻节点，验证新旧修订可并存两轮。
    // 通过 ECS 找回借用地址对应的组件，在唯一的生产线程编辑原始数据。
    const auto* original = context.bpmEvents[1];
    const auto  view     = registry.view<TimelineComponent>();
    for ( auto entity : view ) {
        auto& node = view.get<TimelineComponent>(entity);
        if ( &node == original ) node.m_value = 240.0;
    }
    // 与实际编辑一样由脏标记触发，下一次 ensure 才替换完整修订。
    context.isBpmEventsDirty = true;
    ensureBpmEvents(context);
    // 新槽在发布前接管本次修订，旧槽仍可安全查询前次值。
    nextSnapshot.clear(context.m_beatTimeIndex);
    ensureBpmEvents(context);
    // 再调用一次 ensure，验证同一编辑不会按视口或逻辑轮次重复计数。
    if ( !nextSnapshot.m_beatTimeIndex ||
         context.bpmEventsRevision != firstRevision + 1 ||
         nextSnapshot.m_beatTimeIndex.get() == firstIndex ||
         nextSnapshot.m_beatTimeIndex->m_points.front().m_bpm != 240.0 ||
         oldSnapshot.m_beatTimeIndex->m_points.front().m_bpm != firstBpm )
        return false;

    // 模拟关闭源会话再回收旧快照；快照读取和复用分别遵守自己的生命周期。
    // 会话释放后，新槽的读取仅依赖自己的索引，无需再访问原来的上下文。
    context.m_beatTimeIndex.reset();
    if ( nextSnapshot.m_beatTimeIndex.use_count() != 1 ) return false;
    // 回收路径没有下一个修订，应丢弃旧索引，避免空谱面显示上张图的节奏。
    oldSnapshot.clear();
    return !oldSnapshot.m_beatTimeIndex;
}

}  // namespace

/// @brief 运行跨 BPM 红线的拍号累计测试。
/// @return 全部测试通过时返回 0。
/// @note 逻辑拍线编号与界面拍号索引分别遵守各自的段长取整规则。
int main()
{
    // 用例自行输出实际拍号；失败通过非零退出码传递给测试运行器。
    // 保留现有短路执行顺序，便于先定位基础拍内编号，再检查跨段累计。
    return testBeatIndexWithinSingleBpmSegment() &&
                   testPartialBeatSegmentsAdvanceAtNextBpm() &&
                   testWholeBeatSegmentDoesNotOvercount() &&
                   testBeatTimeIndexRevision()
               ? 0
               : 1;
}
