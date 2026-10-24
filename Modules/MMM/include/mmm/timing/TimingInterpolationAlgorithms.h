#pragma once

#include "mmm/timing/TimingInterpolation.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MMM
{
enum class TimingEffect;
class Timing;

/// @brief 单段最大写出间隔数量，限制不可信文件和编辑输入的内存放大。
inline constexpr std::size_t MAX_TIMING_INTERPOLATION_SAMPLES = 65536;

/// @brief 校验长度、密度、曲线、控制点以及该类型的起终值。
/// @note 同类型段落的重叠检查由拥有完整时间线的逻辑入口处理。
bool isValidTimingInterpolation(const TimingInterpolation& interpolation,
                                TimingEffect effect, double startValue);

/// @brief 按归一化时间直接计算曲线参数，不使用写出采样密度。
/// @warning 渲染与吸附查询热路径；只执行有上限的标量运算。
double evaluateTimingInterpolation(const TimingInterpolation& interpolation,
                                   double startValue, double progress);

/// @brief 在段落内积分参数，超出终点的部分按终值延长。
/// @return 参数乘秒；BPM 调用方再除以 60 转成拍数。
/// @warning 热路径；使用解析原函数与固定上限贝塞尔反解，禁止分配或遍历实体。
double integrateTimingInterpolation(const TimingInterpolation& interpolation,
                                    double startValue, double elapsedSeconds);

/// @brief 编译自定义函数并同时更新两端参数，失败保留原工作副本。
/// @param startValue 成功时写入 f(0)，终值写入段落中的 endValue。
/// @param error 失败原因，成功时清空。
bool setTimingInterpolationFunction(TimingInterpolation& interpolation,
                                    std::string_view     expression,
                                    double& startValue, std::string& error);

/// @brief 返回整个段落的参数范围，非单调函数也能正确显示。
std::pair<double, double> timingInterpolationRange(
    const TimingInterpolation& interpolation, double startValue);

/// @brief 当前自变量定义域长度，单位由 variable 决定。
double timingInterpolationVariableDuration(
    const TimingInterpolation& interpolation);

/// @brief 将段内秒数转换为函数自变量；节拍模式使用独立的 BPM 积分映射。
/// @warning 每帧求值入口；仅二分缓存及有上限的曲线积分，不分配或遍历实体。
double timingInterpolationVariableAtSeconds(
    const TimingInterpolation& interpolation, double elapsedSeconds);

/// @brief 在自变量域内求值，供手绘与拟合画布使用。
/// @warning 每帧绘图入口；借用不可变函数，不复制共享指针或重建映射。
double evaluateTimingInterpolationVariable(
    const TimingInterpolation& interpolation, double startValue,
    double variable);

/// @brief 返回指定输出样本距段首的秒数；尾点精确落在段落终点。
/// @warning 预览与 Jump 查询入口；节拍反解固定迭代，不生成完整采样数组。
double timingInterpolationSampleElapsed(
    const TimingInterpolation& interpolation, std::size_t index);

/// @brief 从完整 BPM 时间线准备节拍映射并锁定派生密度，失败保留原定义。
/// @param startSeconds 段首绝对秒数；timings 中的时间戳仍使用领域毫秒单位。
/// @param fallbackBpm 无红线时使用的有效 BPM，不继承 Scroll/HS 的附带 BPM。
/// @return 分拍、时间范围与自定义函数全域均合法时为真。
/// @warning
/// 仅编辑事件、载入、保存或脏缓存重建调用；会排序和分配，不得逐帧执行。
bool bindTimingInterpolationBeatAxis(TimingInterpolation&       interpolation,
                                     double                     startSeconds,
                                     const std::vector<Timing>& timings,
                                     double fallbackBpm = 120.0);

/// @brief 返回包含两个端点的外部格式写出时间点数量。
std::size_t timingInterpolationSampleCount(
    const TimingInterpolation& interpolation);

/// @brief 把原生段落展开为外部格式独立时间点，普通点保持原样。
/// @details 单段两端均写出；相邻段共用端点时后一个起点优先。
std::vector<Timing> sampleTimingInterpolations(
    const std::vector<Timing>& timings, double fallbackBpm = 120.0);

}  // namespace MMM
