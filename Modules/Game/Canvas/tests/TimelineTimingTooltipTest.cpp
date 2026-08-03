#include "canvas/TimelineTimingTooltip.h"

namespace
{

/// @brief 验证 Timeline Timing 悬浮提示使用编辑器约定术语与单位。
/// @return BPM、SV、Jump 和 HS 的描述均符合界面约定时返回 true。
bool testTimingTooltipDescriptors()
{
    const auto bpm =
        MMM::Canvas::timelineTimingTooltipDescriptor(MMM::TimingEffect::BPM);
    const auto sv =
        MMM::Canvas::timelineTimingTooltipDescriptor(MMM::TimingEffect::SCROLL);
    const auto jump =
        MMM::Canvas::timelineTimingTooltipDescriptor(MMM::TimingEffect::JUMP);
    const auto hs =
        MMM::Canvas::timelineTimingTooltipDescriptor(MMM::TimingEffect::HS);

    return bpm.label == "BPM" && bpm.valueSuffix.empty() && sv.label == "SV" &&
           sv.valueSuffix.empty() && jump.label == "Jump" &&
           jump.valueSuffix == " ms" && hs.label == "HS" &&
           hs.valueSuffix.empty();
}

}  // namespace

/// @brief 运行 Timeline Timing 悬浮提示描述回归测试。
/// @return 全部术语与单位映射正确时返回 0。
int main()
{
    return testTimingTooltipDescriptors() ? 0 : 1;
}
