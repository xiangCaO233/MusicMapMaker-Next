#include "logic/session/SessionUtils.h"

#include "log/colorful-log.h"
#include "logic/ecs/components/TimelineComponent.h"

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

}  // namespace

/// @brief 运行跨 BPM 红线的拍号累计测试。
/// @return 全部测试通过时返回 0。
/// @note 本测试只覆盖有效有序 BPM 输入，不代表异常值或空列表处理已验证。
int main()
{
    // 用例自行输出实际拍号；失败通过非零退出码传递给测试运行器。
    // 保留现有短路执行顺序，便于先定位基础拍内编号，再检查跨段累计。
    return testBeatIndexWithinSingleBpmSegment() &&
                   testPartialBeatSegmentsAdvanceAtNextBpm() &&
                   testWholeBeatSegmentDoesNotOvercount()
               ? 0
               : 1;
}
