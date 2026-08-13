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
MMM::Logic::TimelineComponent makeBpm(double timestamp, double bpm)
{
    return { timestamp, MMM::TimingEffect::BPM, bpm, {} };
}

/// @brief 验证单个 BPM 段内仍按完整拍时长递增拍号。
/// @return 起始拍及下一拍的拍号均正确时返回 true。
bool testBeatIndexWithinSingleBpmSegment()
{
    const auto bpm = makeBpm(10.0, 120.0);
    const std::vector<const MMM::Logic::TimelineComponent*> bpmEvents{ &bpm };

    const int firstBeat =
        MMM::Logic::SessionUtils::calculateBeatIndex(10.49, bpmEvents, 120.0);
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
bool testPartialBeatSegmentsAdvanceAtNextBpm()
{
    const auto first  = makeBpm(10.0, 120.0);
    const auto second = makeBpm(10.1, 180.0);
    const auto third  = makeBpm(10.2, 240.0);
    const std::vector<const MMM::Logic::TimelineComponent*> bpmEvents{ &first,
                                                                       &second,
                                                                       &third };

    const int secondIndex = MMM::Logic::SessionUtils::calculateBeatIndex(
        second.m_timestamp, bpmEvents, 120.0);
    const int thirdIndex = MMM::Logic::SessionUtils::calculateBeatIndex(
        third.m_timestamp, bpmEvents, 120.0);
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
bool testWholeBeatSegmentDoesNotOvercount()
{
    const auto first  = makeBpm(0.0, 120.0);
    const auto second = makeBpm(0.5, 180.0);
    const std::vector<const MMM::Logic::TimelineComponent*> bpmEvents{
        &first, &second
    };

    const int beatIndex = MMM::Logic::SessionUtils::calculateBeatIndex(
        second.m_timestamp, bpmEvents, 120.0);
    if ( beatIndex != 2 ) {
        XERROR("Whole BPM segment overcounted beat index: {}", beatIndex);
        return false;
    }
    return true;
}

}  // namespace

/// @brief 运行跨 BPM 红线的拍号累计测试。
/// @return 全部测试通过时返回 0。
int main()
{
    return testBeatIndexWithinSingleBpmSegment() &&
                   testPartialBeatSegmentsAdvanceAtNextBpm() &&
                   testWholeBeatSegmentDoesNotOvercount()
               ? 0
               : 1;
}
