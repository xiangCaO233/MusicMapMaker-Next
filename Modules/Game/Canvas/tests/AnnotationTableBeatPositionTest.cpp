#include "ui/utils/TimeFormatUtils.h"

#include <string>

namespace
{

/// @brief 校验批注表分列数据与既有组合文本使用同一套拍点换算。
/// @return 拍号、分拍位与组合文本全部一致时返回 true。
bool testSingleBpmSubdivision()
{
    MMM::UI::Utils::CanvasTimeFormatContext context;
    // 120 BPM 下每拍半秒，八分之一秒应落在第一拍的四分之一位置。
    // 这里直接构造批注表缓存持有的数据形态，不依赖活动时间线窗口。
    context.bpmPoints.push_back({ 0.0, 120.0 });
    context.beatDivisor = 4;

    // 结构化结果分别供“拍号”和“分拍位”单元格消费。
    const auto position =
        MMM::UI::Utils::calculateCanvasBeatPosition(0.125, context);
    // 组合格式继续消费结构化结果，防止表格与其它画布标签产生偏差。
    // 分子和分母单独检查，确保表格不会依赖格式化文本拆分字段。
    return position.valid && position.beatNumber == 1 &&
           position.numerator == 1 && position.denominator == 4 &&
           MMM::UI::Utils::TimeFormatDetail::formatBeatTime(0.125, context) ==
               std::string("1 + 1/4");
}

/// @brief 校验跨 BPM 分段后拍号连续累计，分拍按新 BPM 计算。
/// @return 变速后的拍号和分拍位正确时返回 true。
bool testMultipleBpmSegments()
{
    MMM::UI::Utils::CanvasTimeFormatContext context;
    // 第一秒在 120 BPM 下累计两拍，之后 60 BPM 的半秒落在第三拍中点。
    // 节点边界必须先累计完整旧分段，再使用新节点的单拍时长。
    context.bpmPoints   = { { 0.0, 120.0 }, { 1.0, 60.0 } };
    context.beatDivisor = 4;

    // 目标位于第二个节点内部，可同时检验累计拍号与新速率分拍。
    const auto position =
        MMM::UI::Utils::calculateCanvasBeatPosition(1.5, context);
    // 分拍应按 60 BPM 得到二分之一，不能沿用第一段的四分拍偏移。
    return position.valid && position.beatNumber == 3 &&
           position.numerator == 1 && position.denominator == 2;
}

/// @brief 校验缺少 BPM 时返回无效位置，供表格显示明确占位符。
/// @return 无 BPM 时间线被识别为无效位置时返回 true。
bool testMissingBpmTimeline()
{
    const MMM::UI::Utils::CanvasTimeFormatContext context;
    // 空上下文是谱面尚未建立 BPM 数据时的合法状态，不应伪造零拍号。
    const auto position =
        MMM::UI::Utils::calculateCanvasBeatPosition(2.0, context);
    // 表格据此显示短横线，并与首节点前可能出现的零拍号明确区分。
    return !position.valid;
}

}  // namespace

/// @brief 运行批注表拍号与分拍位的纯计算回归用例。
/// @return 全部场景通过时返回 0。
int main()
{
    // 三组场景共同固定分列表格的正常、变速与无计时点行为。
    return testSingleBpmSubdivision() && testMultipleBpmSegments() &&
                   testMissingBpmTimeline()
               ? 0
               : 1;
}
