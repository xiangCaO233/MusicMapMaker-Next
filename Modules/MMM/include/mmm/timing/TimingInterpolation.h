#pragma once

#include <cstddef>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MMM
{
enum class TimingEffect;
class Timing;
class TimingFunction;

/// @brief 插值段落支持的归一化曲线；枚举顺序同时用于编辑器下拉选择。
enum class TimingCurve {
    Linear,
    EaseIn,
    EaseOut,
    SmoothStep,
    Sine,
    Exponential,
    Bezier,
    Cubic,
    Quartic,
    Quintic,
    SquareRoot,
    CubeRoot,
    Logarithmic,
    Reciprocal,
    SineIn,
    SineOut,
    HyperbolicTangent,
    Custom,
};

/// @brief 一个时间点后的连续参数变化，时间长度统一使用秒。
/// @details 起始值由所属 Timing 提供；原生格式保留本结构，外部格式只写采样。
struct TimingInterpolation {
    /// @brief 段落持续时间；不随 Timing 模型的毫秒单位转换。
    double m_duration{ 1.0 };
    /// @brief 段落终点参数值。
    double m_endValue{ 1.0 };
    /// @brief 外部格式写出密度，单位为每秒采样次数。
    double m_samplesPerSecond{ 16.0 };
    /// @brief 段落采用的归一化曲线。
    TimingCurve m_curve{ TimingCurve::Linear };
    /// @brief 贝塞尔第一控制点的时间比例。
    double m_controlX1{ 0.25 };
    /// @brief 贝塞尔第一控制点的参数比例。
    double m_controlY1{ 0.25 };
    /// @brief 贝塞尔第二控制点的时间比例。
    double m_controlX2{ 0.75 };
    /// @brief 贝塞尔第二控制点的参数比例。
    double m_controlY2{ 0.75 };
    /// @brief 自定义绝对参数函数；预设曲线不使用此缓存。
    /// @warning 不可变程序跨逻辑与 UI 快照共享，避免实体删除后旧快照悬空。
    /// 只在描述复制时保留所有权；逐次求值必须借用引用，不复制指针。
    std::shared_ptr<const TimingFunction> m_function;
    /// @brief 按源表达式比较语义，不比较编译缓存地址。
    bool operator==(const TimingInterpolation&) const;
};

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

/// @brief 返回包含两个端点的外部格式写出时间点数量。
std::size_t timingInterpolationSampleCount(
    const TimingInterpolation& interpolation);

/// @brief 把原生段落展开为外部格式独立时间点，普通点保持原样。
/// @details 单段两端均写出；相邻段共用端点时后一个起点优先。
std::vector<Timing> sampleTimingInterpolations(
    const std::vector<Timing>& timings);

/// @brief 序列化段落定义；时间与密度单位固定为秒和每秒次数。
void to_json(nlohmann::json& json, const TimingInterpolation& interpolation);

/// @brief 从不可信 JSON 读取段落；字段类型或曲线非法时返回空。
std::optional<TimingInterpolation> readTimingInterpolation(
    const nlohmann::json& json);
}  // namespace MMM
