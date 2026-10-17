#include "canvas/TimelineTimingTooltip.h"
#include "canvas/TimelineFormatSupport.h"
#include "canvas/TimelineStopDuration.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>

namespace
{

/// @brief 验证 Timeline Timing 悬浮提示使用编辑器约定术语与单位。
/// @return BPM、SV、Jump 和 HS 的描述均符合界面约定时返回 true。
bool testTimingTooltipDescriptors()
{
    // 分别取得四种现有 Timing 类型的静态描述。
    const auto bpm =
        MMM::Canvas::timelineTimingTooltipDescriptor(MMM::TimingEffect::BPM);
    const auto sv =
        MMM::Canvas::timelineTimingTooltipDescriptor(MMM::TimingEffect::SCROLL);
    const auto jump =
        MMM::Canvas::timelineTimingTooltipDescriptor(MMM::TimingEffect::JUMP);
    // Jump 是唯一附带物理单位的现有类型，需独立检查其空格与单位文本。
    const auto hs =
        MMM::Canvas::timelineTimingTooltipDescriptor(MMM::TimingEffect::HS);
    // HS 与 SV 保留编辑器通用缩写，不在提示层展开内部枚举名称。

    // BPM、SV 与 HS 直接展示无单位参数；Jump 的原始数值按毫秒解释。
    // 同时比较标签和后缀，避免界面术语正确但单位悄然回归。
    return bpm.label == "BPM" && bpm.valueSuffix.empty() && sv.label == "SV" &&
           sv.valueSuffix.empty() && jump.label == "Jump" &&
           jump.valueSuffix == " ms" && hs.label == "HS" &&
           hs.valueSuffix.empty();
}

/// @brief 验证 STOP 输入单位、往返换算和可提交范围。
/// @return 与 iBMSC 数值定义及秒数草稿约束一致时返回 true。
/// @note iBMSC Form1.vb 的 #STOP 导入乘一万、导出除一万，仅作定点存储。
/// 一小节 192 单位，对应标准四拍；因此 48 单位是一拍。
/// @note 不依赖真实音频设备，本场景只验证参数换算，不宣称完成听感验收。
bool testStopDurationUnits()
{
    using namespace MMM::Canvas;
    // 120 BPM 每拍半秒：48 单位半秒、192 单位两秒。
    // 多组 BPM 防止错误实现把 STOP 单位直接固定为毫秒。
    for ( double bpm : { 60.0, 120.0, 180.0, 240.0 } ) {
        const double seconds = stopDurationSeconds(48.0, bpm, true);
        if ( std::abs(seconds - 60.0 / bpm) > 1e-12 ) return false;
        // 切换单位本身不改变秒数，往返误差限制在 double 舍入范围。
        if ( std::abs(stopDurationDisplayValue(seconds, bpm, true) - 48.0) >
             1e-10 )
            return false;
        // 普通模式不使用 BPM，所有输入都按原始秒数解释。
        if ( stopDurationSeconds(0.375, bpm, false) != 0.375 ||
             stopDurationDisplayValue(0.375, bpm, false) != 0.375 )
            return false;
    }
    // 标准小节覆盖用户常见的 192 输入，验证不是整拍或整小节偏差。
    if ( stopDurationSeconds(192.0, 120.0, true) != 2.0 ) return false;
    // 清空、负号和小数输入的中间态不能在转换层被强制钳制。
    // 提交校验单独拒绝它们，保证用户能从零重新输入一个正数。
    if ( stopDurationSeconds(0.0, 120.0, true) != 0.0 ||
         stopDurationSeconds(-48.0, 120.0, true) != -0.5 )
        return false;
    // 极短正时长保持原精度，不再被每帧改写成一微秒。
    // 无穷与 NaN 不能成为播放层等待帧数的来源。
    return isValidStopDuration(1e-8) && !isValidStopDuration(0.0) &&
           !isValidStopDuration(-1.0) &&
           !isValidStopDuration(std::numeric_limits<double>::infinity()) &&
           !isValidStopDuration(std::numeric_limits<double>::quiet_NaN());
}

}  // namespace

/// @brief 验证三个外部格式的完整支持矩阵及扩展名边界。
/// @return 区分原格式保存能力且未知格式沿用原行为时返回 true。
/// @details 同一编辑器标签可切换格式，因此每次查询必须根据当前路径判断。
/// 测试不依赖真实文件，避免把目录存在与否混入格式识别结果。
/// mmm 是无损保存的推荐格式，不能继承前一个外部格式的限制。
/// mc 的四种当前类型都受支持，不能错误套用 osu 的子集。
/// IMD 大写后缀是 Windows 常见输入，必须与小写格式同等处理。
bool testTimelineFormatSupport()
{
    using MMM::TimingEffect;
    constexpr std::array effects{ TimingEffect::BPM,
                                  TimingEffect::SCROLL,
                                  TimingEffect::JUMP,
                                  TimingEffect::HS };
    // 每一种类型都单独验证，避免只测试各格式的首个支持项。
    // 使用四个固定枚举，防止默认类型使测试跳过 Jump 与 HS。
    // 混合大小写与不同路径分隔符分别覆盖两种来源路径。
    for ( std::size_t i = 0; i < effects.size(); ++i ) {
        if ( MMM::Canvas::timelineFormatSupport("folder/chart.IMD")
                 .supports(effects[i]) != (i == 0) )
            return false;
        if ( MMM::Canvas::timelineFormatSupport("C:\\maps\\chart.OsU")
                 .supports(effects[i]) != (i < 2) )
            return false;
        if ( !MMM::Canvas::timelineFormatSupport("chart.mc")
                  .supports(effects[i]) )
            return false;
        if ( !MMM::Canvas::timelineFormatSupport("chart.mmm")
                  .supports(effects[i]) )
            return false;
    }
    // 不从目录、备份后缀或未知格式推测限制，保护其他导入流程。
    // 文件名没有扩展名时，目录里的 .osu 不能决定保存能力。
    // 备份文件按最后一个后缀识别，不能因路径包含 osu 而误警告。
    // BMS 已有明确能力矩阵，未知后缀仍保持原有宽松提示。
    // 空路径覆盖尚未获得文件名的谱面，保持原有编辑行为。
    for ( const auto path : { "folder.osu/chart", "chart.osu.bak", "" } ) {
        if ( !MMM::Canvas::timelineFormatSupport(path).extension.empty() )
            return false;
    }
    // BMS 只持久化 BPM 与 STOP，等待单位明确为秒。
    // 外部不支持格式必须提示；原生 MMM 则保留冻结事件。
    // 等待能力独立于 Jump/HS，不能因为支持瞬时跳转就声称支持冻结。
    // 大写后缀验证识别规则与 Windows 文件名一致。
    // 秒单位防止同表格中的 Jump 毫秒说明被复用。
    const auto bms = MMM::Canvas::timelineFormatSupport("chart.BMS");
    const auto stop =
        MMM::Canvas::timelineTimingTooltipDescriptor(TimingEffect::STOP);
    if ( !bms.supports(TimingEffect::BPM) ||
         !bms.supports(TimingEffect::STOP) ||
         bms.supports(TimingEffect::SCROLL) || bms.supports(TimingEffect::HS) ||
         bms.supports(TimingEffect::JUMP) || stop.label != "STOP" ||
         stop.valueSuffix != " s" ||
         MMM::Canvas::timelineFormatSupport("chart.mc")
             .supports(TimingEffect::STOP) ||
         !MMM::Canvas::timelineFormatSupport("chart.mmm")
              .supports(TimingEffect::STOP) )
        return false;
    // 文案列表必须与矩阵一致，且标准格式保留所有当前时间点类型。
    return MMM::Canvas::timelineFormatSupport("chart.imd").supportedTypes ==
               "BPM" &&
           MMM::Canvas::timelineFormatSupport("chart.osu").supportedTypes ==
               "BPM、SCROLL" &&
           MMM::Canvas::timelineFormatSupport("chart.mc").supportedTypes ==
               "BPM、SCROLL、JUMP、HS";
}

/// @brief 运行 Timeline Timing 悬浮提示描述回归测试。
/// @return 全部术语与单位映射正确时返回 0。
/// @note 同时核对提示使用的格式能力，不依赖 ImGui 或真实谱面文件。
int main()
{
    // 函数为 constexpr 纯映射，运行测试仍能覆盖实际头文件调用结果。
    return testTimingTooltipDescriptors() && testTimelineFormatSupport() &&
                   testStopDurationUnits()
               ? 0
               : 1;
}
